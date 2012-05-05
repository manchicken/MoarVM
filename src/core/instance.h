/* The various "bootstrap" types, based straight off of some core
 * representations. They are used during the 6model bootstrap. */
struct _MVMBootTypes {
    MVMObject *BOOTStr;
    MVMObject *BOOTArray;
    MVMObject *BOOTHash;
    MVMObject *BOOTCCode;
    MVMObject *BOOTCode;
};

/* Represents a MoarVM instance. */
typedef struct _MVMInstance {
    /* The list of active threads. */
    MVMThreadContext **threads;
    
    /* The number of active threads. */
    MVMuint16 num_threads;
    
    /* The MoarVM-level NULL. */
    MVMObject *null;
    
    /* The KnowHOW meta-object; all other meta-objects (which are
     * built in user-space) are built out of this. */
    MVMObject *KnowHOW;
    
    /* Set of bootstrapping types. */
    struct _MVMBootTypes *boot_types;
    
    /* An array mapping representation IDs to function tables. */
    MVMREPROps **repr_registry;

    /* Number of representations registered so far. */
    MVMuint32 num_reprs;

    /* Hash mapping representation names to IDs. */
    apr_pool_t *repr_name_to_id_pool;
    apr_hash_t *repr_name_to_id_hash;
} MVMInstance;
