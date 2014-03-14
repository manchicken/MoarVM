#include "moar.h"

/* This representation's function pointer table. */
static const MVMREPROps this_repr;

/* Locates all of the attributes. Puts them onto a flattened, ordered
 * list of attributes (populating the passed flat_list). Also builds
 * the index mapping for doing named lookups. Note index is not related
 * to the storage position. */
static MVMObject * index_mapping_and_flat_list(MVMThreadContext *tc, MVMObject *mro, MVMCStructREPRData *repr_data) {
    MVMInstance *instance  = tc->instance;
    MVMObject *flat_list, *class_list, *attr_map_list;
    MVMint32  num_classes, i, current_slot = 0;
    MVMCStructNameMap *result;

    MVMint32 mro_idx = MVM_repr_elems(tc, mro);

    MVM_gc_root_temp_push(tc, (MVMCollectable **)&mro);

    flat_list = MVM_repr_alloc_init(tc, MVM_hll_current(tc)->slurpy_array_type);
    MVM_gc_root_temp_push(tc, (MVMCollectable **)&flat_list);

    class_list = MVM_repr_alloc_init(tc, MVM_hll_current(tc)->slurpy_array_type);
    MVM_gc_root_temp_push(tc, (MVMCollectable **)&class_list);

    attr_map_list = MVM_repr_alloc_init(tc, MVM_hll_current(tc)->slurpy_array_type);
    MVM_gc_root_temp_push(tc, (MVMCollectable **)&attr_map_list);

    /* Walk through the parents list. */
    while (mro_idx)
    {
        /* Get current class in MRO. */
        MVMObject *type_info     = MVM_repr_at_pos_o(tc, mro, --mro_idx);
        MVMObject *current_class = MVM_repr_at_pos_o(tc, type_info, 0);

        /* Get its local parents; make sure we're not doing MI. */
        MVMObject *parents     = MVM_repr_at_pos_o(tc, type_info, 2);
        MVMint32  num_parents = MVM_repr_elems(tc, parents);
        if (num_parents <= 1) {
            /* Get attributes and iterate over them. */
            MVMObject *attributes     = MVM_repr_at_pos_o(tc, type_info, 1);
            MVMIter * const attr_iter = (MVMIter *)MVM_iter(tc, attributes);
            MVMObject *attr_map = NULL;

            if (MVM_iter_istrue(tc, attr_iter)) {
                MVM_gc_root_temp_push(tc, (MVMCollectable **)&attr_iter);
                attr_map = MVM_repr_alloc_init(tc, MVM_hll_current(tc)->slurpy_hash_type);
                MVM_gc_root_temp_push(tc, (MVMCollectable **)&attr_map);
            }

            while (MVM_iter_istrue(tc, attr_iter)) {
                MVMObject *current_slot_obj = MVM_repr_box_int(tc, MVM_hll_current(tc)->int_box_type, current_slot);
                MVMObject *attr, *name_obj;
                MVMString *name;

                MVM_repr_shift_o(tc, (MVMObject *)attr_iter);

                /* Get attribute. */
                attr = MVM_iterval(tc, attr_iter);

                /* Get its name. */
                name_obj = MVM_repr_at_key_o(tc, attr, instance->str_consts.name);
                name     = MVM_repr_get_str(tc, name_obj);

                MVM_repr_bind_key_o(tc, attr_map, name, current_slot_obj);

                current_slot++;

                /* Push attr onto the flat list. */
                MVM_repr_push_o(tc, flat_list, attr);
            }

            if (attr_map) {
                MVM_gc_root_temp_pop_n(tc, 2);
            }

            /* Add to class list and map list. */
            MVM_repr_push_o(tc, class_list, current_class);
            MVM_repr_push_o(tc, attr_map_list, attr_map);
        }
        else {
            MVM_exception_throw_adhoc(tc,
                "CStruct representation does not support multiple inheritance");
        }
    }

    MVM_gc_root_temp_pop_n(tc, 4);

    /* We can now form the name map. */
    num_classes = MVM_repr_elems(tc, class_list);
    result = (MVMCStructNameMap *) malloc(sizeof(MVMCStructNameMap) * (1 + num_classes));

    for (i = 0; i < num_classes; i++) {
        result[i].class_key = MVM_repr_at_pos_o(tc, class_list, i);
        result[i].name_map  = MVM_repr_at_pos_o(tc, attr_map_list, i);
    }

    /* set the end to be NULL, it's useful for iteration. */
    result[i].class_key = NULL;

    repr_data->name_to_index_mapping = result;

    return flat_list;
}

/* This works out an allocation strategy for the object. It takes care of
 * "inlining" storage of attributes that are natively typed, as well as
 * noting unbox targets. */
static void compute_allocation_strategy(MVMThreadContext *tc, MVMObject *repr_info, MVMCStructREPRData *repr_data) {
    /* Compute index mapping table and get flat list of attributes. */
    MVMObject *flat_list = index_mapping_and_flat_list(tc, repr_info, repr_data);

    /* If we have no attributes in the index mapping, then just the header. */
    if (repr_data->name_to_index_mapping[0].class_key == NULL) {
        repr_data->struct_size = 1; /* avoid 0-byte malloc */
    }

    /* Otherwise, we need to compute the allocation strategy.  */
    else {
        /* We track the size of the struct, which is what we'll want offsets into. */
        MVMint32 cur_size = 0;

        /* Get number of attributes and set up various counters. */
        MVMint32 num_attrs        = MVM_repr_elems(tc, flat_list);
        MVMint32 info_alloc       = num_attrs == 0 ? 1 : num_attrs;
        MVMint32 cur_obj_attr     = 0;
        MVMint32 cur_str_attr     = 0;
        MVMint32 cur_init_slot    = 0;
        MVMint32 i;

        /* Allocate location/offset arrays and GC mark info arrays. */
        repr_data->num_attributes      = num_attrs;
        repr_data->attribute_locations = (MVMint32 *)   malloc(info_alloc * sizeof(MVMint32));
        repr_data->struct_offsets      = (MVMint32 *)   malloc(info_alloc * sizeof(MVMint32));
        repr_data->flattened_stables   = (MVMSTable **) calloc(info_alloc, sizeof(MVMObject *));
        repr_data->member_types        = (MVMObject **) calloc(info_alloc, sizeof(MVMObject *));

        /* Go over the attributes and arrange their allocation. */
        for (i = 0; i < num_attrs; i++) {
            /* Fetch its type; see if it's some kind of unboxed type. */
            MVMObject *attr  = MVM_repr_at_pos_o(tc, flat_list, i);
            MVMObject *type  = MVM_repr_at_key_o(tc, attr, tc->instance->str_consts.type);
            MVMint32   bits  = sizeof(void *) * 8;
            MVMint32   align = ALIGNOF(void *);
            if (type) {
                /* See if it's a type that we know how to handle in a C struct. */
                MVMStorageSpec spec = REPR(type)->get_storage_spec(tc, STABLE(type));
                MVMint32  type_id   = REPR(type)->ID;
                if (spec.inlineable == MVM_STORAGE_SPEC_INLINED &&
                        (spec.boxed_primitive == MVM_STORAGE_SPEC_BP_INT ||
                         spec.boxed_primitive == MVM_STORAGE_SPEC_BP_NUM)) {
                    /* It's a boxed int or num; pretty easy. It'll just live in the
                     * body of the struct. Instead of masking in i here (which
                     * would be the parallel to how we handle boxed types) we
                     * repurpose it to store the bit-width of the type, so
                     * that get_attribute_ref can find it later. */
                    bits = spec.bits;
                    /* align = spec.align; */

                    if (bits % 8) {
                         MVM_exception_throw_adhoc(tc,
                            "CStruct only supports native types that are a multiple of 8 bits wide (was passed: %ld)", bits);
                    }

                    repr_data->attribute_locations[i] = (bits << MVM_CSTRUCT_ATTR_SHIFT) | MVM_CSTRUCT_ATTR_IN_STRUCT;
                    repr_data->flattened_stables[i] = STABLE(type);
                    if (REPR(type)->initialize) {
                        if (!repr_data->initialize_slots)
                            repr_data->initialize_slots = (MVMint32 *) calloc(info_alloc + 1, sizeof(MVMint32));
                        repr_data->initialize_slots[cur_init_slot] = i;
                        cur_init_slot++;
                    }
                }
                else if (spec.can_box & MVM_STORAGE_SPEC_CAN_BOX_STR) {
                    /* It's a string of some kind.  */
                    repr_data->num_child_objs++;
                    repr_data->attribute_locations[i] = (cur_obj_attr++ << MVM_CSTRUCT_ATTR_SHIFT) | MVM_CSTRUCT_ATTR_STRING;
                    repr_data->member_types[i] = type;
                }
                else if (type_id == MVM_REPR_ID_MVMCArray) {
                    /* It's a CArray of some kind.  */
                    repr_data->num_child_objs++;
                    repr_data->attribute_locations[i] = (cur_obj_attr++ << MVM_CSTRUCT_ATTR_SHIFT) | MVM_CSTRUCT_ATTR_CARRAY;
                    repr_data->member_types[i] = type;
                }
                else if (type_id == MVM_REPR_ID_MVMCStruct) {
                    /* It's a CStruct. */
                    repr_data->num_child_objs++;
                    repr_data->attribute_locations[i] = (cur_obj_attr++ << MVM_CSTRUCT_ATTR_SHIFT) | MVM_CSTRUCT_ATTR_CSTRUCT;
                    repr_data->member_types[i] = type;
                }
                else if (type_id == MVM_REPR_ID_MVMCPointer) {
                    /* It's a CPointer. */
                    repr_data->num_child_objs++;
                    repr_data->attribute_locations[i] = (cur_obj_attr++ << MVM_CSTRUCT_ATTR_SHIFT) | MVM_CSTRUCT_ATTR_CPTR;
                    repr_data->member_types[i] = type;
                }
                else {
                    MVM_exception_throw_adhoc(tc,
                        "CStruct representation only handles int, num, CArray, CPointer and CStruct");
                }
            }
            else {
                MVM_exception_throw_adhoc(tc,
                    "CStruct representation requires the types of all attributes to be specified");
            }

            /* Do allocation. */
            /* C structure needs careful alignment. If cur_size is not aligned
             * to align bytes (cur_size % align), make sure it is before we
             * add the next element. */
            if (cur_size % align) {
                cur_size += align - cur_size % align;
            }

            repr_data->struct_offsets[i] = cur_size;
            cur_size += bits / 8;
        }

        /* Finally, put computed allocation size in place; it's body size plus
         * header size. Also number of markables and sentinels. */
        repr_data->struct_size = cur_size;
        if (repr_data->initialize_slots)
            repr_data->initialize_slots[cur_init_slot] = -1;
    }
}

/* Helper for reading an int at the specified offset. */
static MVMint32 get_int_at_offset(void *data, MVMint32 offset) {
    void *location = (char *)data + offset;
    return *((MVMint32 *)location);
}

/* Helper for writing an int at the specified offset. */
static void set_int_at_offset(void *data, MVMint32 offset, MVMint32 value) {
    void *location = (char *)data + offset;
    *((MVMint32 *)location) = value;
}

/* Helper for reading a num at the specified offset. */
static MVMnum32 get_num_at_offset(void *data, MVMint32 offset) {
    void *location = (char *)data + offset;
    return *((MVMnum32 *)location);
}

/* Helper for writing a num at the specified offset. */
static void set_num_at_offset(void *data, MVMint32 offset, MVMnum32 value) {
    void *location = (char *)data + offset;
    *((MVMnum32 *)location) = value;
}

/* Helper for reading a pointer at the specified offset. */
static void * get_ptr_at_offset(void *data, MVMint32 offset) {
    void *location = (char *)data + offset;
    return *((void **)location);
}

/* Helper for writing a pointer at the specified offset. */
static void set_ptr_at_offset(void *data, MVMint32 offset, void *value) {
    void *location = (char *)data + offset;
    *((void **)location) = value;
}

/* Helper for finding a slot number. */
static MVMint32 try_get_slot(MVMThreadContext *tc, MVMCStructREPRData *repr_data, MVMObject *class_key, MVMString *name) {
    if (repr_data->name_to_index_mapping) {
        MVMCStructNameMap *cur_map_entry = repr_data->name_to_index_mapping;
        while (cur_map_entry->class_key != NULL) {
            if (cur_map_entry->class_key == class_key) {
                MVMObject *slot_obj = MVM_repr_at_key_o(tc, cur_map_entry->name_map, name);
                if (IS_CONCRETE(slot_obj))
                    return MVM_repr_get_int(tc, slot_obj);
                break;
            }
            cur_map_entry++;
        }
    }
    return -1;
}

/* Creates a new type object of this representation, and associates it with
 * the given HOW. */
static MVMObject * type_object_for(MVMThreadContext *tc, MVMObject *HOW) {
    MVMSTable *st  = MVM_gc_allocate_stable(tc, &this_repr, HOW);

    MVMROOT(tc, st, {
        MVMObject *obj = MVM_gc_allocate_type_object(tc, st);
        MVM_ASSIGN_REF(tc, &(st->header), st->WHAT, obj);
        st->size = sizeof(MVMCStruct);
    });

    return st->WHAT;
}

/* Composes the representation. */
static void compose(MVMThreadContext *tc, MVMSTable *st, MVMObject *repr_info) {
    /* Compute allocation strategy. */
    MVMCStructREPRData *repr_data = calloc(1, sizeof(MVMCStructREPRData));
    MVMObject *attr_info = MVM_repr_at_key_o(tc, repr_info, tc->instance->str_consts.attribute);
    compute_allocation_strategy(tc, attr_info, repr_data);
    st->REPR_data = repr_data;
}

/* Initialize a new instance. */
static void initialize(MVMThreadContext *tc, MVMSTable *st, MVMObject *root, void *data) {
    MVMCStructREPRData * repr_data = (MVMCStructREPRData *)st->REPR_data;

    /* Allocate object body. */
    MVMCStructBody *body = (MVMCStructBody *)data;
    body->cstruct = malloc(repr_data->struct_size > 0 ? repr_data->struct_size : 1);
    memset(body->cstruct, 0, repr_data->struct_size);

    /* Allocate child obj array. */
    if (repr_data->num_child_objs > 0)
        body->child_objs = (MVMObject **)calloc(repr_data->num_child_objs,
            sizeof(MVMObject *));

    /* Initialize the slots. */
    if (repr_data->initialize_slots) {
        MVMint32 i;
        for (i = 0; repr_data->initialize_slots[i] >= 0; i++) {
            MVMint32  offset = repr_data->struct_offsets[repr_data->initialize_slots[i]];
            MVMSTable *st     = repr_data->flattened_stables[repr_data->initialize_slots[i]];
            st->REPR->initialize(tc, st, root, (char *)body->cstruct + offset);
        }
    }
}

/* Copies to the body of one object to another. */
static void copy_to(MVMThreadContext *tc, MVMSTable *st, void *src, MVMObject *dest_root, void *dest) {
    MVMCStructREPRData * repr_data = (MVMCStructREPRData *) st->REPR_data;
    MVMCStructBody *src_body = (MVMCStructBody *)src;
    MVMCStructBody *dest_body = (MVMCStructBody *)dest;
    /* XXX todo */
    /* XXX also need to shallow copy the obj array */
}

/* Helper for complaining about attribute access errors. */
MVM_NO_RETURN static void no_such_attribute(MVMThreadContext *tc, const char *action, MVMObject *class_handle, MVMString *name) MVM_NO_RETURN_GCC;
static void no_such_attribute(MVMThreadContext *tc, const char *action, MVMObject *class_handle, MVMString *name) {
    MVM_exception_throw_adhoc(tc,
        "Can not %s non-existent attribute '%s'",
        action, name);
}

/* Helper to die because this type doesn't support attributes. */
MVM_NO_RETURN static void die_no_attrs(MVMThreadContext *tc) MVM_NO_RETURN_GCC;
static void die_no_attrs(MVMThreadContext *tc) {
    MVM_exception_throw_adhoc(tc,
            "CStruct representation attribute not yet fully implemented");
}

static void get_attribute(MVMThreadContext *tc, MVMSTable *st, MVMObject *root,
        void *data, MVMObject *class_handle, MVMString *name, MVMint64 hint,
        MVMRegister *result_reg, MVMuint16 kind) {
    MVM_exception_throw_adhoc(tc, "NYI");
}

/* Binds the given value to the specified attribute. */
static void bind_attribute(MVMThreadContext *tc, MVMSTable *st, MVMObject *root,
        void *data, MVMObject *class_handle, MVMString *name, MVMint64 hint,
        MVMRegister value_reg, MVMuint16 kind) {
    MVM_exception_throw_adhoc(tc, "NYI");
}


/* Checks if an attribute has been initialized. */
static MVMint64 is_attribute_initialized(MVMThreadContext *tc, MVMSTable *st, void *data, MVMObject *class_handle, MVMString *name, MVMint64 hint) {
    die_no_attrs(tc);
}

/* Gets the hint for the given attribute ID. */
static MVMint64 hint_for(MVMThreadContext *tc, MVMSTable *st, MVMObject *class_handle, MVMString *name) {
    return MVM_NO_HINT;
}

/* Adds held objects to the GC worklist. */
static void gc_mark(MVMThreadContext *tc, MVMSTable *st, void *data, MVMGCWorklist *worklist) {
    MVMCStructREPRData *repr_data = (MVMCStructREPRData *) st->REPR_data;
    MVMCStructBody *body = (MVMCStructBody *)data;
    MVMint32 i;
    for (i = 0; i < repr_data->num_child_objs; i++)
        MVM_gc_worklist_add(tc, worklist, &body->child_objs[i]);
}

/* Marks the representation data in an STable.*/
static void gc_mark_repr_data(MVMThreadContext *tc, MVMSTable *st, MVMGCWorklist *worklist) {
    MVMCStructREPRData *repr_data = (MVMCStructREPRData *) st->REPR_data;
    MVMCStructNameMap *map = repr_data->name_to_index_mapping;
    if (map) {
        MVMint32 i;
        for (i = 0; map[i].class_key; i++) {
            MVM_gc_worklist_add(tc, worklist, &map[i].class_key);
            MVM_gc_worklist_add(tc, worklist, &map[i].name_map);
        }
    }
}

/* This is called to do any cleanup of resources when an object gets
 * embedded inside another one. Never called on a top-level object. */
static void gc_cleanup(MVMThreadContext *tc, MVMSTable *st, void *data) {
    MVMCStructBody *body = (MVMCStructBody *)data;
    if (body->child_objs)
        free(body->child_objs);
    if (body->cstruct)
        free(body->cstruct);
}

/* Called by the VM in order to free memory associated with this object. */
static void gc_free(MVMThreadContext *tc, MVMObject *obj) {
	gc_cleanup(tc, STABLE(obj), OBJECT_BODY(obj));
}

/* Gets the storage specification for this representation. */
static MVMStorageSpec get_storage_spec(MVMThreadContext *tc, MVMSTable *st) {
    MVMStorageSpec spec;
    spec.inlineable = MVM_STORAGE_SPEC_REFERENCE;
    spec.boxed_primitive = MVM_STORAGE_SPEC_BP_NONE;
    spec.can_box = 0;
    spec.bits = sizeof(void *) * 8;
    return spec;
}

/* Serializes the REPR data. */
static void serialize_repr_data(MVMThreadContext *tc, MVMSTable *st, MVMSerializationWriter *writer) {
    MVMCStructREPRData *repr_data = (MVMCStructREPRData *)st->REPR_data;
    /* Could do this, but can also re-compute it each time for now. */
}

/* Deserializes the REPR data. */
static void deserialize_repr_data(MVMThreadContext *tc, MVMSTable *st, MVMSerializationReader *reader) {
    /* Just allocating it will do for now. */
    st->REPR_data = calloc(1, sizeof(MVMCStructREPRData));
}

/* Initializes the representation. */
const MVMREPROps * MVMCStruct_initialize(MVMThreadContext *tc) {
    return &this_repr;
}

static const MVMREPROps this_repr = {
    type_object_for,
    MVM_gc_allocate_object,
    initialize,
    copy_to,
    {
        get_attribute,
        bind_attribute,
        hint_for,
        is_attribute_initialized
    },   /* attr_funcs */
    MVM_REPR_DEFAULT_BOX_FUNCS,
    MVM_REPR_DEFAULT_POS_FUNCS,
    MVM_REPR_DEFAULT_ASS_FUNCS,
    MVM_REPR_DEFAULT_ELEMS,
    get_storage_spec,
    NULL, /* change_type */
    NULL, /* serialize */
    NULL, /* deserialize */
    serialize_repr_data,
    deserialize_repr_data,
    NULL, /* deserialize_stable_size */
    gc_mark,
    gc_free,
    gc_cleanup,
    NULL, /* gc_mark_repr_data */
    NULL, /* gc_free_repr_data */
    compose,
    "CStruct", /* name */
    MVM_REPR_ID_MVMCStruct,
    0, /* refs_frames */
};