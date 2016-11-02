#include <stdio.h>
#include <stdlib.h> // strtol
#include <string.h> // strerror
#include <unistd.h> // STDIN_FILENO
#include <anyrtc.h>

/* TODO: Replace with zf_log */
#define DEBUG_MODULE "redirect-transport-app"
#define DEBUG_LEVEL 7
#include <re_dbg.h>

#define EOE(code) exit_on_error(code, __FILE__, __LINE__)
#define EOR(code) exit_on_re_error(code, __FILE__, __LINE__)
#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L) || (__GNUC__ >= 3)
#define EWE(...) exit_with_error(__FILE__, __LINE__, __VA_ARGS__)
#elif defined(__GNUC__)
#define EWE(args...) exit_with_error(__FILE__, __LINE__, args)
#endif

enum {
    PARAMETERS_MAX_LENGTH = 8192,
};

struct parameters {
    struct anyrtc_ice_parameters* ice_parameters;
    struct anyrtc_ice_candidates* ice_candidates;
    struct anyrtc_dtls_parameters* dtls_parameters;
};

struct client {
    char* name;
    struct anyrtc_ice_gather_options* gather_options;
    struct sa redirect_address;
    enum anyrtc_ice_role ice_role;
    struct anyrtc_certificate* certificate;
    struct anyrtc_ice_gatherer* gatherer;
    struct anyrtc_ice_transport* ice_transport;
    struct anyrtc_dtls_transport* dtls_transport;
    struct anyrtc_sctp_transport* redirect_transport;
    struct parameters* local_parameters;
    struct parameters* remote_parameters;
};

static void before_exit() {
    // Close
    anyrtc_close();

    // Check memory leaks
    tmr_debug();
    mem_debug();
}

static void exit_on_error(
        enum anyrtc_code code,
        char const* const file,
        uint32_t line
) {
    switch (code) {
        case ANYRTC_CODE_SUCCESS:
            return;
        case ANYRTC_CODE_NOT_IMPLEMENTED:
            DEBUG_WARNING("Not implemented in %s %"PRIu32"\n", file, line);
            return;
        default:
            DEBUG_WARNING("Error in %s %"PRIu32" (%d): %s\n",
                          file, line, code, anyrtc_code_to_str(code));
            before_exit();
            exit((int) code);
    }
}

static void exit_on_re_error(
        int code,
        char const* const file,
        uint32_t line
) {
    if (code != 0) {
        DEBUG_WARNING("Error in %s %"PRIu32" (%d): %s\n", file, line, code, strerror(code));
        before_exit();
        exit(code);
    }
}

static void exit_with_error(
        char const* const file,
        uint32_t line,
        char const* const formatter,
        ...
) {
    char* message;

    // Format message
    va_list ap;
    va_start(ap, formatter);
    re_vsdprintf(&message, formatter, ap);
    va_end(ap);

    // Print message
    DEBUG_WARNING("%s %"PRIu32": %s\n", file, line, message);

    // Dereference & bye
    mem_deref(message);
    before_exit();
    exit(2);
}

static bool str_to_uint16(
        uint16_t* const numberp,
        char* const str
) {
    char* end;
    int_least32_t number = (int_least32_t) strtol(str, &end, 10);

    // Don't ask, strtol is insane...
    if (number == 0 && str == end) {
        return false;
    }

    // Check bounds
    if (number < 0 || number > UINT16_MAX) {
        return false;
    }

    // Phew, we did it...
    *numberp = (uint16_t) number;
    return true;
}

static enum anyrtc_code dict_get_entry(
        void* const valuep,
        struct odict* const parent,
        char* const key,
        enum odict_type const type
) {
    struct odict_entry const * entry;

    // Check arguments
    if (!valuep || !parent || !key) {
        return ANYRTC_CODE_INVALID_ARGUMENT;
    }

    // Do lookup
    entry = odict_lookup(parent, key);

    // Check for entry
    if (!entry) {
        DEBUG_WARNING("'%s' missing\n", key);
        return ANYRTC_CODE_INVALID_ARGUMENT;
    }

    // Check for type
    if (entry->type != type) {
        DEBUG_WARNING("'%s' is not a string\n", key);
        return ANYRTC_CODE_INVALID_ARGUMENT;
    }

    // Set value according to type
    switch (type) {
        case ODICT_OBJECT:
        case ODICT_ARRAY:
            *((struct odict** const) valuep) = entry->u.odict;
            break;
        case ODICT_STRING:
            *((char** const) valuep) = entry->u.str;
            break;
        case ODICT_INT:
            *((int* const) valuep) = entry->u.integer;
            break;
        case ODICT_DOUBLE:
            *((double* const) valuep) = entry->u.dbl;
            break;
        case ODICT_BOOL:
            *((bool* const) valuep) = entry->u.boolean;
            break;
        case ODICT_NULL:
            *((char** const) valuep) = NULL; // meh!
            break;
        default:
            return ANYRTC_CODE_INVALID_ARGUMENT;
    }

    // Done
    return ANYRTC_CODE_SUCCESS;
}

static enum anyrtc_code dict_get_uint32(
        uint32_t* const valuep,
        struct odict* const parent,
        char* const key
) {
    int_least32_t value;

    // Check arguments
    if (!valuep || !parent || !key) {
        return ANYRTC_CODE_INVALID_ARGUMENT;
    }

    // Get int_least32_t
    enum anyrtc_code error = dict_get_entry(&value, parent, key, ODICT_INT);
    if (error) {
        return error;
    }

    // Check bounds
    if (value < 0 || value > UINT32_MAX) {
        return ANYRTC_CODE_INVALID_ARGUMENT;
    } else {
        *valuep = (uint32_t) value;
        return ANYRTC_CODE_SUCCESS;
    }
}

static enum anyrtc_code dict_get_uint16(
        uint16_t* const valuep,
        struct odict* const parent,
        char* const key
) {
    int_least32_t value;

    // Check arguments
    if (!valuep || !parent || !key) {
        return ANYRTC_CODE_INVALID_ARGUMENT;
    }

    // Get int_least32_t
    enum anyrtc_code error = dict_get_entry(&value, parent, key, ODICT_INT);
    if (error) {
        return error;
    }

    // Check bounds
    if (value < 0 || value > UINT16_MAX) {
        return ANYRTC_CODE_INVALID_ARGUMENT;
    } else {
        *valuep = (uint16_t) value;
        return ANYRTC_CODE_SUCCESS;
    }
}

static void client_exchange_parameters(
        struct client* client
);

static void ice_gatherer_state_change_handler(
        enum anyrtc_ice_gatherer_state const state, // read-only
        void* const arg
) {
    struct client* const client = arg;
    char const * const state_name = anyrtc_ice_gatherer_state_to_name(state);
    (void) arg;
    DEBUG_PRINTF("(%s) ICE gatherer state: %s\n", client->name, state_name);
}

static void ice_gatherer_error_handler(
        struct anyrtc_ice_candidate* const host_candidate, // read-only, nullable
        char const * const url, // read-only
        uint16_t const error_code, // read-only
        char const * const error_text, // read-only
        void* const arg
) {
    struct client* const client = arg;
    (void) host_candidate; (void) error_code; (void) arg;
    DEBUG_PRINTF("(%s) ICE gatherer error, URL: %s, reason: %s\n", client->name, url, error_text);
}

static void ice_gatherer_local_candidate_handler(
        struct anyrtc_ice_candidate* const candidate,
        char const * const url, // read-only
        void* const arg
) {
    struct client* const client = arg;
    (void) candidate; (void) arg;

    if (candidate) {
        DEBUG_PRINTF("(%s) ICE gatherer local candidate, URL: %s\n", client->name, url);

        // Start exchanging parameters
        client_exchange_parameters(client);
    } else {
        DEBUG_PRINTF("(%s) ICE gatherer last local candidate\n", client->name);
    }
}

static void ice_transport_state_change_handler(
        enum anyrtc_ice_transport_state const state,
        void* const arg
) {
    struct client* const client = arg;
    char const * const state_name = anyrtc_ice_transport_state_to_name(state);
    (void) arg;
    DEBUG_PRINTF("(%s) ICE transport state: %s\n", client->name, state_name);
}

static void ice_transport_candidate_pair_change_handler(
        struct anyrtc_ice_candidate* const local, // read-only
        struct anyrtc_ice_candidate* const remote, // read-only
        void* const arg
) {
    struct client* const client = arg;
    (void) local; (void) remote;
    DEBUG_PRINTF("(%s) ICE transport candidate pair change\n", client->name);
}

static void dtls_transport_state_change_handler(
        enum anyrtc_dtls_transport_state const state, // read-only
        void* const arg
) {
    struct client* const client = arg;
    char const * const state_name = anyrtc_dtls_transport_state_to_name(state);
    DEBUG_PRINTF("(%s) DTLS transport state change: %s\n", client->name, state_name);
}

static void dtls_transport_error_handler(
        /* TODO: error.message (probably from OpenSSL) */
        void* const arg
) {
    struct client* const client = arg;
    // TODO: Print error message
    DEBUG_PRINTF("(%s) DTLS transport error: %s\n", client->name, "???");
}

static void signal_handler(
        int sig
) {
    DEBUG_INFO("Got signal: %d, terminating...\n", sig);
    re_cancel();
}

static void client_init(
        struct client* const client
) {
    // Generate certificates
    EOE(anyrtc_certificate_generate(&client->certificate, NULL));
    struct anyrtc_certificate* certificates[] = {client->certificate};

    // Create ICE gatherer
    EOE(anyrtc_ice_gatherer_create(
            &client->gatherer, client->gather_options,
            ice_gatherer_state_change_handler, ice_gatherer_error_handler,
            ice_gatherer_local_candidate_handler, client));

    // Create ICE transport
    EOE(anyrtc_ice_transport_create(
            &client->ice_transport, client->gatherer,
            ice_transport_state_change_handler, ice_transport_candidate_pair_change_handler,
            client));

    // Create DTLS transport
    EOE(anyrtc_dtls_transport_create(
            &client->dtls_transport, client->ice_transport, certificates,
            sizeof(certificates) / sizeof(struct anyrtc_certificate*),
            dtls_transport_state_change_handler, dtls_transport_error_handler, client));

    // Create redirect transport
//    EOE(anyrtc_redirect_transport_create(
//            &client->redirect_transport, client->dtls_transport, 0, data_channel_handler, client));
}

static void client_start_gathering(
        struct client* const client
) {
    // Start gathering
    EOE(anyrtc_ice_gatherer_gather(client->gatherer, NULL));
}

static void client_exchange_parameters(
        struct client* client
) {
    // TODO PRINT
}

static void client_get_parameters(
        struct client* const client
) {
    struct parameters* const local_parameters = client->local_parameters;

    // Get local ICE parameters
    EOE(anyrtc_ice_gatherer_get_local_parameters(
            &local_parameters->ice_parameters, client->gatherer));

    // Get local ICE candidates
    EOE(anyrtc_ice_gatherer_get_local_candidates(
            &local_parameters->ice_candidates, client->gatherer));

    // Get local DTLS parameters
    EOE(anyrtc_dtls_transport_get_local_parameters(
            &local_parameters->dtls_parameters, client->dtls_transport));
}

static void client_set_parameters(
        struct client* const client
) {
    struct parameters* const remote_parameters = client->remote_parameters;

    // Set remote ICE candidates
    EOE(anyrtc_ice_transport_set_remote_candidates(
            client->ice_transport, remote_parameters->ice_candidates->candidates,
            remote_parameters->ice_candidates->n_candidates));
}

static void client_start_transports(
        struct client* const client
) {
    struct parameters* const remote_parameters = client->remote_parameters;

    // Start ICE transport
    EOE(anyrtc_ice_transport_start(
            client->ice_transport, client->gatherer, remote_parameters->ice_parameters,
            client->ice_role));

    // Start DTLS transport
    EOE(anyrtc_dtls_transport_start(
            client->dtls_transport, remote_parameters->dtls_parameters));
}

static void parameters_destroy(
        struct parameters* const parameters
) {
    // Dereference
    parameters->ice_parameters = mem_deref(parameters->ice_parameters);
    parameters->ice_candidates = mem_deref(parameters->ice_candidates);
    parameters->dtls_parameters = mem_deref(parameters->dtls_parameters);
}

static void client_stop(
        struct client* const client
) {
    // Stop transports & close gatherer
//    EOE(anyrtc_redirect_transport_stop(client->redirect_transport));
    EOE(anyrtc_dtls_transport_stop(client->dtls_transport));
    EOE(anyrtc_ice_transport_stop(client->ice_transport));
    EOE(anyrtc_ice_gatherer_close(client->gatherer));

    // Dereference & close
    parameters_destroy(client->remote_parameters);
    parameters_destroy(client->local_parameters);
    client->redirect_transport = mem_deref(client->redirect_transport);
    client->dtls_transport = mem_deref(client->dtls_transport);
    client->ice_transport = mem_deref(client->ice_transport);
    client->gatherer = mem_deref(client->gatherer);
    client->certificate = mem_deref(client->certificate);
    client->gather_options = mem_deref(client->gather_options);
}

static enum anyrtc_code client_get_ice_parameters(
        struct anyrtc_ice_parameters** const parametersp,
        struct odict* const dict
) {
    enum anyrtc_code error = ANYRTC_CODE_SUCCESS;
    char* username_fragment;
    char* password;
    bool ice_lite;

    // Get ICE parameters
    error |= dict_get_entry(&username_fragment, dict, "usernameFragment", ODICT_STRING);
    error |= dict_get_entry(&password, dict, "password", ODICT_STRING);
    error |= dict_get_entry(&ice_lite, dict, "iceLite", ODICT_BOOL);
    if (error) {
        return error;
    }

    // Create ICE parameters instance
    return anyrtc_ice_parameters_create(parametersp, username_fragment, password, ice_lite);
}

static void client_ice_candidates_destroy(
        void* const arg
) {
    struct anyrtc_ice_candidates* const candidates = arg;
    size_t i;

    // Dereference each item
    for (i = 0; i < candidates->n_candidates; ++i) {
        mem_deref(candidates->candidates[i]);
    }
}

static enum anyrtc_code client_get_ice_candidates(
        struct anyrtc_ice_candidates** const candidatesp,
        struct odict* const dict
) {
    size_t n;
    struct anyrtc_ice_candidates* candidates;
    enum anyrtc_code error = ANYRTC_CODE_SUCCESS;
    struct le* le;
    size_t i;

    // Get length
    n = list_count(&dict->lst);

    // Allocate & set length immediately
    candidates = mem_zalloc(
            sizeof(struct anyrtc_ice_candidates) + (sizeof(struct anyrtc_ice_candidate*) * n),
            client_ice_candidates_destroy);
    if (!candidates) {
        EWE("No memory to allocate ICE candidates array");
    }
    candidates->n_candidates = n;

    // Get ICE candidates
    for (le = list_head(&dict->lst), i = 0; le != NULL; le = le->next, ++i) {
        char* foundation;
        uint32_t priority;
        char* ip;
        char const* protocol_str = NULL;
        enum anyrtc_ice_protocol protocol;
        uint16_t port;
        char const* type_str = NULL;
        enum anyrtc_ice_candidate_type type;
        char const* tcp_type_str = NULL;
        enum anyrtc_ice_tcp_candidate_type tcp_type = ANYRTC_ICE_TCP_CANDIDATE_TYPE_ACTIVE;
        char* related_address = NULL;
        uint16_t related_port = 0;

        // Get ICE candidate
        error |= dict_get_entry(&foundation, dict, "foundation", ODICT_STRING);
        error |= dict_get_uint32(&priority, dict, "priority");
        error |= dict_get_entry(&ip, dict, "ip", ODICT_STRING);
        error |= dict_get_entry(&protocol_str, dict, "protocol", ODICT_STRING);
        error |= anyrtc_str_to_ice_protocol(&protocol, protocol_str);
        error |= dict_get_uint16(&port, dict, "port");
        error |= dict_get_entry(&type_str, dict, "type", ODICT_STRING);
        error |= anyrtc_str_to_ice_candidate_type(&type, type_str);
        if (protocol == ANYRTC_ICE_PROTOCOL_TCP) {
            error |= dict_get_entry(&tcp_type_str, dict, "tcpType", ODICT_STRING);
            error |= anyrtc_str_to_ice_tcp_candidate_type(&tcp_type, tcp_type_str);
        }
        dict_get_entry(&related_address, dict, "relatedAddress", ODICT_STRING);
        dict_get_uint16(&related_port, dict, "relatedPort");
        if (error) {
            goto out;
        }

        // Create and add ICE candidate
        error = anyrtc_ice_candidate_create(
                &candidates->candidates[i], foundation, priority, ip, protocol, port, type,
                tcp_type, related_address, related_port);
        if (error) {
            goto out;
        }
    }

out:
    if (error) {
        mem_deref(candidates);
    } else {
        // Set pointer
        *candidatesp = candidates;
    }
    return error;
}

static void client_dtls_fingerprints_destroy(
        void* const arg
) {
    struct anyrtc_dtls_fingerprints* const fingerprints = arg;
    size_t i;

    // Dereference each item
    for (i = 0; i < fingerprints->n_fingerprints; ++i) {
        mem_deref(fingerprints->fingerprints[i]);
    }
}

static enum anyrtc_code client_get_dtls_parameters(
        struct anyrtc_dtls_parameters** const parametersp,
        struct odict* const dict
) {
    size_t n;
    struct anyrtc_dtls_parameters* parameters = NULL;
    struct anyrtc_dtls_fingerprints* fingerprints;
    enum anyrtc_code error = ANYRTC_CODE_SUCCESS;
    char const* role_str = NULL;
    enum anyrtc_dtls_role role;
    struct odict* node;
    struct le* le;
    size_t i;

    // Get length
    n = list_count(&dict->lst);

    // Allocate & set length immediately
    fingerprints = mem_zalloc(
            sizeof(struct anyrtc_ice_candidates) + (sizeof(struct anyrtc_ice_candidate*) * n),
            client_dtls_fingerprints_destroy);
    if (!fingerprints) {
        EWE("No memory to allocate DTLS fingerprint array");
    }
    fingerprints->n_fingerprints = n;

    // Get DTLS role
    error |= dict_get_entry(&role_str, dict, "role", ODICT_STRING);
    error |= anyrtc_str_to_dtls_role(&role, role_str);
    if (error) {
        role = ANYRTC_DTLS_ROLE_AUTO;
    }
    error |= dict_get_entry(&node, dict, "fingerprints", ODICT_ARRAY);
    if (error) {
        goto out;
    }

    // Get fingerprints
    for (le = list_head(&node->lst); le != NULL; le = le->next) {
        char* algorithm_str = NULL;
        enum anyrtc_certificate_sign_algorithm algorithm;
        char* value;

        // Get fingerprint
        error |= dict_get_entry(&algorithm_str, dict, "algorithm", ODICT_STRING);
        error |= anyrtc_str_to_certificate_sign_algorithm(&algorithm, algorithm_str);
        error |= dict_get_entry(&value, dict, "value", ODICT_STRING);
        if (error) {
            goto out;
        }

        // Create and add fingerprint
        error = anyrtc_dtls_fingerprint_create(&fingerprints->fingerprints[i], algorithm, value);
        if (error) {
            goto out;
        }
    }

    // Create DTLS parameters
    error = anyrtc_dtls_parameters_create(
            &parameters, role, fingerprints->fingerprints, fingerprints->n_fingerprints);

out:
    mem_deref(fingerprints);

    if (!error) {
        // Set pointer
        *parametersp = parameters;
    }
    return error;
}

static void client_stdin_handler(
        int flags,
        void* const arg
) {
    struct client* const client = arg;
    char buffer[PARAMETERS_MAX_LENGTH];
    size_t length;
    bool do_exit = false;
    struct odict* dict = NULL;
    struct odict* node = NULL;
    enum anyrtc_code error = ANYRTC_CODE_SUCCESS;
    struct anyrtc_ice_parameters* ice_parameters = NULL;
    struct anyrtc_ice_candidates* ice_candidates = NULL;
    struct anyrtc_dtls_parameters* dtls_parameters = NULL;
    (void) flags;

    // Get message from stdin
    if (!fgets((char*) buffer, PARAMETERS_MAX_LENGTH, stdin)) {
        EWE("Error polling stdin");
    }
    length = strlen(buffer);

    // Exit?
    if (length == 1 && buffer[0] == '\n') {
        do_exit = true;
        DEBUG_NOTICE("Exiting\n");
        goto out;
    }

    // Print JSON
    DEBUG_INFO("Remote Parameters: %H", odict_debug, dict);

    // Decode JSON
    EOR(json_decode_odict(&dict, 16, buffer, length, 1));
    error |= dict_get_entry(&node, dict, "iceParameters", ODICT_OBJECT);
    error |= client_get_ice_parameters(&ice_parameters, node);
    error |= dict_get_entry(&node, dict, "iceCandidates", ODICT_ARRAY);
    error |= client_get_ice_candidates(&ice_candidates, node);
    error |= dict_get_entry(&node, dict, "dtlsParameters", ODICT_OBJECT);
    error |= client_get_dtls_parameters(&dtls_parameters, node);

    // Set parameters & start transports
    if (!error) {
        client->remote_parameters->ice_parameters = mem_ref(ice_parameters);
        client->remote_parameters->ice_candidates = mem_ref(ice_candidates);
        client->remote_parameters->dtls_parameters = mem_ref(dtls_parameters);
        client_set_parameters(client);
        client_start_transports(client);
    }
    
out:
    mem_deref(dtls_parameters);
    mem_deref(ice_candidates);
    mem_deref(ice_parameters);
    mem_deref(dict);
    
    // Exit?
    if (do_exit) {
        // Stop client & bye
        client_stop(client);
        before_exit();
        exit(0);
    }
}

static void exit_with_usage(char* program) {
    DEBUG_WARNING("Usage: %s <0|1 (ice-role)> <redirect-ip> <redirect-sctp_port>", program);
    exit(1);
}

int main(int argc, char* argv[argc + 1]) {
    enum anyrtc_ice_role ice_role;
    uint16_t redirect_port;
    struct anyrtc_ice_gather_options* gather_options;
    char* const stun_google_com_urls[] = {"stun.l.google.com:19302", "stun1.l.google.com:19302"};
    char* const turn_zwuenf_org_urls[] = {"turn.zwuenf.org"};
    struct client client = {0};

    // Initialise
    EOE(anyrtc_init());

    // Debug
    // TODO: This should be replaced by our own debugging system
    dbg_init(DBG_DEBUG, DBG_ALL);
    DEBUG_PRINTF("Init\n");

    // Check arguments length
    if (argc < 4) {
        exit_with_usage(argv[0]);
    }

    // Get ICE role
    switch (argv[1][0]) {
        case '0':
            ice_role = ANYRTC_ICE_ROLE_CONTROLLED;
            break;
        case '1':
            ice_role = ANYRTC_ICE_ROLE_CONTROLLING;
            break;
        default:
            exit_with_usage(argv[0]);
            return 1;
    }

    // Get redirect port
    if (!str_to_uint16(&redirect_port, argv[3])) {
        exit_with_usage(argv[0]);
    }

    // Get redirect IP
    if (!sa_set_str(&client.redirect_address, argv[2], redirect_port)) {
        exit_with_usage(argv[0]);
    }

    // Create ICE gather options
    EOE(anyrtc_ice_gather_options_create(&gather_options, ANYRTC_ICE_GATHER_ALL));

    // Add ICE servers to ICE gather options
    EOE(anyrtc_ice_gather_options_add_server(
            gather_options, stun_google_com_urls,
            sizeof(stun_google_com_urls) / sizeof(char*),
            NULL, NULL, ANYRTC_ICE_CREDENTIAL_NONE));
    EOE(anyrtc_ice_gather_options_add_server(
            gather_options, turn_zwuenf_org_urls,
            sizeof(turn_zwuenf_org_urls) / sizeof(char*),
            "bruno", "onurb", ANYRTC_ICE_CREDENTIAL_PASSWORD));

    // Set client fields
    client.name = "A";
    client.gather_options = gather_options;
    client.ice_role = ice_role;

    // Setup client
    client_init(&client);

    // Start gathering
    client_start_gathering(&client);

    // Listen on stdin
    EOR(fd_listen(STDIN_FILENO, FD_READ, client_stdin_handler, &client));

    // Start main loop
    // TODO: Wrap re_main?
    // TODO: Stop main loop once gathering is complete
    EOE(anyrtc_error_to_code(re_main(signal_handler)));

    // Stop client & bye
    client_stop(&client);
    before_exit();
    return 0;
}