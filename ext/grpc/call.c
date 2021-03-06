/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "call.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <php.h>
#include <php_ini.h>
#include <ext/standard/info.h>
#include <ext/spl/spl_exceptions.h>
#include "php_grpc.h"
#include "call_credentials.h"

#include <zend_exceptions.h>
#include <zend_hash.h>

#include <stdbool.h>

#include <grpc/support/alloc.h>
#include <grpc/grpc.h>

#include "completion_queue.h"
#include "timeval.h"
#include "channel.h"
#include "byte_buffer.h"

zend_class_entry *grpc_ce_call;

static zend_object_handlers call_object_handlers_call;

/* Frees and destroys an instance of wrapped_grpc_call */
static void free_wrapped_grpc_call(zend_object *object) {
  wrapped_grpc_call *call = wrapped_grpc_call_from_obj(object);
  if (call->owned && call->wrapped != NULL) {
    grpc_call_destroy(call->wrapped);
  }
  zend_object_std_dtor(&call->std);
}

/* Initializes an instance of wrapped_grpc_call to be associated with an object
 * of a class specified by class_type */
zend_object *create_wrapped_grpc_call(zend_class_entry *class_type) {
  wrapped_grpc_call *intern;
  intern = ecalloc(1, sizeof(wrapped_grpc_call) +
                   zend_object_properties_size(class_type));

  zend_object_std_init(&intern->std, class_type);
  object_properties_init(&intern->std, class_type);
  
  intern->std.handlers = &call_object_handlers_call;
  
  return &intern->std;
}

/* Wraps a grpc_call struct in a PHP object. Owned indicates whether the struct
   should be destroyed at the end of the object's lifecycle */
void grpc_php_wrap_call(grpc_call *wrapped, bool owned, zval *call_object) {
  object_init_ex(call_object, grpc_ce_call);
  wrapped_grpc_call *call = Z_WRAPPED_GRPC_CALL_P(call_object);
  call->wrapped = wrapped;
  call->owned = owned;
}

/* Creates and returns a PHP array object with the data in a
 * grpc_metadata_array. Returns NULL on failure */
void grpc_parse_metadata_array(grpc_metadata_array *metadata_array, zval *array) {
  int count = metadata_array->count;
  grpc_metadata *elements = metadata_array->metadata;
  int i;
  zval *data;
  HashTable *array_hash;
  zval inner_array;
  char *str_key;
  char *str_val;
  size_t key_len;
  
  array_init(array);
  array_hash = HASH_OF(array);
  grpc_metadata *elem;
  for (i = 0; i < count; i++) {
    elem = &elements[i];
    key_len = strlen(elem->key);
    str_key = ecalloc(key_len + 1, sizeof(char));
    memcpy(str_key, elem->key, key_len);
    str_val = ecalloc(elem->value_length + 1, sizeof(char));
    memcpy(str_val, elem->value, elem->value_length);
    if ((data = zend_hash_str_find(array_hash, str_key, key_len)) != NULL) {
      if (Z_TYPE_P(data) != IS_ARRAY) {
        zend_throw_exception(zend_exception_get_default(),
                             "Metadata hash somehow contains wrong types.",
                             1);
        efree(str_key);
        efree(str_val);
        return;
      }
      add_next_index_stringl(data, str_val, elem->value_length);
    } else {
      array_init(&inner_array);
      add_next_index_stringl(&inner_array, str_val, elem->value_length);
      add_assoc_zval(array, str_key, &inner_array);
    }
  }
}

/* Populates a grpc_metadata_array with the data in a PHP array object.
   Returns true on success and false on failure */
bool create_metadata_array(zval *array, grpc_metadata_array *metadata) {
  zval *inner_array;
  zval *value;
  HashTable *array_hash;
  //HashPosition array_pointer;
  HashTable *inner_array_hash;
  //HashPosition inner_array_pointer;
  zend_string *key;
  //zend_ulong index;
  if (Z_TYPE_P(array) != IS_ARRAY) {
    return false;
  }
  grpc_metadata_array_init(metadata);
  array_hash = HASH_OF(array);

  ZEND_HASH_FOREACH_STR_KEY_VAL(array_hash, key, inner_array) {
  /*for (zend_hash_internal_pointer_reset_ex(array_hash, &array_pointer);
       (inner_array = zend_hash_get_current_data_ex(array_hash,
                                                    &array_pointer)) != NULL;
       zend_hash_move_forward_ex(array_hash, &array_pointer)) {
    if (zend_hash_get_current_key_ex(array_hash, &key, &index,
                                     &array_pointer) != HASH_KEY_IS_STRING) {
      return false;
    }*/
    if (key == NULL) {
      return false;
    }
    if (Z_TYPE_P(inner_array) != IS_ARRAY) {
      return false;
    }
    inner_array_hash = HASH_OF(inner_array);
    metadata->capacity += zend_hash_num_elements(inner_array_hash);
  }
  ZEND_HASH_FOREACH_END();

  metadata->metadata = gpr_malloc(metadata->capacity * sizeof(grpc_metadata));
 
  ZEND_HASH_FOREACH_STR_KEY_VAL(array_hash, key, inner_array) {
  /*for (zend_hash_internal_pointer_reset_ex(array_hash, &array_pointer);
       (inner_array = zend_hash_get_current_data_ex(array_hash,
                                                    &array_pointer)) != NULL;
       zend_hash_move_forward_ex(array_hash, &array_pointer)) {
    if (zend_hash_get_current_key_ex(array_hash, &key, &index,
                                     &array_pointer) != HASH_KEY_IS_STRING) {
      return false;
    }*/
    if (key == NULL) {
      return false;
    }
    inner_array_hash = HASH_OF(inner_array);

    ZEND_HASH_FOREACH_VAL(inner_array_hash, value) {
    /*for (zend_hash_internal_pointer_reset_ex(inner_array_hash,
                                             &inner_array_pointer);
         (value = zend_hash_get_current_data_ex(inner_array_hash,
                                                &inner_array_pointer)) != NULL;
         zend_hash_move_forward_ex(inner_array_hash, &inner_array_pointer)) {*/
      if (Z_TYPE_P(value) != IS_STRING) {
        return false;
      }
      metadata->metadata[metadata->count].key = ZSTR_VAL(key);
      metadata->metadata[metadata->count].value = Z_STRVAL_P(value);
      metadata->metadata[metadata->count].value_length = Z_STRLEN_P(value);
      metadata->count += 1;
    } ZEND_HASH_FOREACH_END();
  } ZEND_HASH_FOREACH_END();
  return true;
}

/**
 * Constructs a new instance of the Call class.
 * @param Channel $channel The channel to associate the call with. Must not be
 *     closed.
 * @param string $method The method to call
 * @param Timeval $absolute_deadline The deadline for completing the call
 */
PHP_METHOD(Call, __construct) {
  wrapped_grpc_call *call = Z_WRAPPED_GRPC_CALL_P(getThis());
  zval *channel_obj;
  zend_string *method;
  zval *deadline_obj;
  zend_string *host_override;

  /* "OSO|S" == 1 Object, 1 string, 1 Object, 1 optional string */
#ifndef FAST_ZPP
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "OSO|S", &channel_obj,
                            grpc_ce_channel, &method, &deadline_obj,
                            grpc_ce_timeval, &host_override) == FAILURE) {
    zend_throw_exception(
        spl_ce_InvalidArgumentException,
        "Call expects a Channel, a String, a Timeval and an optional String",
        1);
    return;
  }
#else
  ZEND_PARSE_PARAMETERS_START(3, 4)
    Z_PARAM_OBJECT_OF_CLASS(channel_obj, grpc_ce_channel)
    Z_PARAM_STR(method)
    Z_PARAM_OBJECT_OF_CLASS(deadline_obj, grpc_ce_timeval)
    Z_PARAM_OPTIONAL
    Z_PARAM_STR(host_override)
  ZEND_PARSE_PARAMETERS_END();
#endif

  wrapped_grpc_channel *channel = Z_WRAPPED_GRPC_CHANNEL_P(channel_obj);
  if (channel->wrapped == NULL) {
    zend_throw_exception(spl_ce_InvalidArgumentException,
                         "Call cannot be constructed from a closed Channel",
                         1);
    return;
  }
  add_property_zval(getThis(), "channel", channel_obj);
  wrapped_grpc_timeval *deadline = Z_WRAPPED_GRPC_TIMEVAL_P(deadline_obj);
  call->wrapped = grpc_channel_create_call(
      channel->wrapped, NULL, GRPC_PROPAGATE_DEFAULTS, completion_queue,
      ZSTR_VAL(method), host_override == NULL ? NULL : ZSTR_VAL(host_override),
      deadline->wrapped, NULL);
  call->owned = true;
}

/**
 * Start a batch of RPC actions.
 * @param array batch Array of actions to take
 * @return object Object with results of all actions
 */
PHP_METHOD(Call, startBatch) {
  wrapped_grpc_call *call = Z_WRAPPED_GRPC_CALL_P(getThis());
  grpc_op ops[8];
  size_t op_num = 0;
  zval *array;
  zval *value;
  zval *inner_value;
  HashTable *array_hash;
  //HashPosition array_pointer;
  HashTable *status_hash;
  HashTable *message_hash;
  zval *message_value;
  zval *message_flags;
  zend_string *key;
  zend_ulong index;
  
  grpc_metadata_array metadata;
  grpc_metadata_array trailing_metadata;
  grpc_metadata_array recv_metadata;
  grpc_metadata_array recv_trailing_metadata;
  grpc_status_code status;
  char *status_details = NULL;
  size_t status_details_capacity = 0;
  grpc_byte_buffer *message;
  int cancelled;
  grpc_call_error error;
  char *message_str;
  size_t message_len;
  zval recv_status;
  
  grpc_metadata_array_init(&metadata);
  grpc_metadata_array_init(&trailing_metadata);
  grpc_metadata_array_init(&recv_metadata);
  grpc_metadata_array_init(&recv_trailing_metadata);
  object_init(return_value);
  memset(ops, 0, sizeof(ops));

  /* "a" == 1 array */
#ifndef FAST_ZPP
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "a", &array) == FAILURE) {
    zend_throw_exception(spl_ce_InvalidArgumentException,
                         "start_batch expects an array", 1);
    goto cleanup;
  }
#else
  ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_ARRAY(array)
  ZEND_PARSE_PARAMETERS_END();
#endif

  array_hash = HASH_OF(array);
  ZEND_HASH_FOREACH_KEY_VAL(array_hash, index, key, value) {
  /*for (zend_hash_internal_pointer_reset_ex(array_hash, &array_pointer);
       (value = zend_hash_get_current_data_ex(array_hash,
                                              &array_pointer)) != NULL;
       zend_hash_move_forward_ex(array_hash, &array_pointer)) {
    if (zend_hash_get_current_key_ex(array_hash, &key, &index,
                                     &array_pointer) != HASH_KEY_IS_LONG) {
      zend_throw_exception(spl_ce_InvalidArgumentException,
                           "batch keys must be integers", 1);
      goto cleanup;
    }*/
    if (key) {
      zend_throw_exception(spl_ce_InvalidArgumentException,
                              "batch keys must be integers", 1);
      goto cleanup;
    }

    switch(index) {
      case GRPC_OP_SEND_INITIAL_METADATA:
        if (!create_metadata_array(value, &metadata)) {
          zend_throw_exception(spl_ce_InvalidArgumentException,
                               "Bad metadata value given", 1);
          goto cleanup;
        }
        ops[op_num].data.send_initial_metadata.count = metadata.count;
        ops[op_num].data.send_initial_metadata.metadata = metadata.metadata;
        break;
      case GRPC_OP_SEND_MESSAGE:
        if (Z_TYPE_P(value) != IS_ARRAY) {
          zend_throw_exception(spl_ce_InvalidArgumentException,
                               "Expected an array for send message", 1);
          goto cleanup;
        }
        message_hash = HASH_OF(value);
        if ((message_flags = zend_hash_str_find(message_hash, "flags",
                                                sizeof("flags") - 1)) != NULL) {
          if (Z_TYPE_P(message_flags) != IS_LONG) {
            zend_throw_exception(spl_ce_InvalidArgumentException,
                                 "Expected an int for message flags", 1);
          }
          ops[op_num].flags = Z_LVAL_P(message_flags) & GRPC_WRITE_USED_MASK;
        }
        if ((message_value = zend_hash_str_find(
            message_hash, "message", sizeof("message") - 1)) == NULL ||
            Z_TYPE_P(message_value) != IS_STRING) {
          zend_throw_exception(spl_ce_InvalidArgumentException,
                               "Expected a string for send message", 1);
          goto cleanup;
        }
        ops[op_num].data.send_message =
            string_to_byte_buffer(Z_STRVAL_P(message_value),
                                  Z_STRLEN_P(message_value));
        break;
      case GRPC_OP_SEND_CLOSE_FROM_CLIENT:
        break;
      case GRPC_OP_SEND_STATUS_FROM_SERVER:
        status_hash = HASH_OF(value);
        if ((inner_value = zend_hash_str_find(
            status_hash, "metadata", sizeof("metadata") - 1)) != NULL) {
          if (!create_metadata_array(inner_value, &trailing_metadata)) {
            zend_throw_exception(spl_ce_InvalidArgumentException,
                                 "Bad trailing metadata value given", 1);
            goto cleanup;
          }
          ops[op_num].data.send_status_from_server.trailing_metadata =
              trailing_metadata.metadata;
          ops[op_num].data.send_status_from_server.trailing_metadata_count =
              trailing_metadata.count;
        }
        if ((inner_value = zend_hash_str_find(
            status_hash, "code", sizeof("code") - 1)) != NULL) {
          if (Z_TYPE_P(inner_value) != IS_LONG) {
            zend_throw_exception(spl_ce_InvalidArgumentException,
                                 "Status code must be an integer", 1);
            goto cleanup;
          }
          ops[op_num].data.send_status_from_server.status =
              Z_LVAL_P(inner_value);
        } else {
          zend_throw_exception(spl_ce_InvalidArgumentException,
                               "Integer status code is required", 1);
          goto cleanup;
        }
        if ((inner_value = zend_hash_str_find(
            status_hash, "details", sizeof("details") - 1)) != NULL) {
          if (Z_TYPE_P(inner_value) != IS_STRING) {
            zend_throw_exception(spl_ce_InvalidArgumentException,
                                 "Status details must be a string", 1);
            goto cleanup;
          }
          ops[op_num].data.send_status_from_server.status_details =
              Z_STRVAL_P(inner_value);
        } else {
          zend_throw_exception(spl_ce_InvalidArgumentException,
                               "String status details is required", 1);
          goto cleanup;
        }
        break;
      case GRPC_OP_RECV_INITIAL_METADATA:
        ops[op_num].data.recv_initial_metadata = &recv_metadata;
        break;
      case GRPC_OP_RECV_MESSAGE:
        ops[op_num].data.recv_message = &message;
        break;
      case GRPC_OP_RECV_STATUS_ON_CLIENT:
        ops[op_num].data.recv_status_on_client.trailing_metadata =
            &recv_trailing_metadata;
        ops[op_num].data.recv_status_on_client.status = &status;
        ops[op_num].data.recv_status_on_client.status_details =
            &status_details;
        ops[op_num].data.recv_status_on_client.status_details_capacity =
            &status_details_capacity;
        break;
      case GRPC_OP_RECV_CLOSE_ON_SERVER:
        ops[op_num].data.recv_close_on_server.cancelled = &cancelled;
        break;
      default:
        zend_throw_exception(spl_ce_InvalidArgumentException,
                             "Unrecognized key in batch", 1);
        goto cleanup;
    }
    ops[op_num].op = (grpc_op_type)index;
    ops[op_num].flags = 0;
    ops[op_num].reserved = NULL;
    op_num++;
  }
  ZEND_HASH_FOREACH_END();

  error = grpc_call_start_batch(call->wrapped, ops, op_num,
                                call->wrapped, NULL);
  if (error != GRPC_CALL_OK) {
    zend_throw_exception(spl_ce_LogicException,
                         "start_batch was called incorrectly",
                         (long)error);
    goto cleanup;
  }
  grpc_completion_queue_pluck(completion_queue, call->wrapped,
                              gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
  for (int i = 0; i < op_num; i++) {
    switch(ops[i].op) {
      case GRPC_OP_SEND_INITIAL_METADATA:
        add_property_bool(return_value, "send_metadata", true);
        break;
      case GRPC_OP_SEND_MESSAGE:
        add_property_bool(return_value, "send_message", true);
        break;
      case GRPC_OP_SEND_CLOSE_FROM_CLIENT:
        add_property_bool(return_value, "send_close", true);
        break;
      case GRPC_OP_SEND_STATUS_FROM_SERVER:
        add_property_bool(return_value, "send_status", true);
        break;
      case GRPC_OP_RECV_INITIAL_METADATA:
        grpc_parse_metadata_array(&recv_metadata, array);
        add_property_zval(return_value, "metadata", array);
        //TODO(thinkerou): right?
        /*if (Z_REFCOUNT_P(array) == 1) {
          ZVAL_UNREF(array);
        } else {
          Z_DELREF_P(array);
        }*/
        break;
      case GRPC_OP_RECV_MESSAGE:
        byte_buffer_to_string(message, &message_str, &message_len);
        if (message_str == NULL) {
          add_property_null(return_value, "message");
        } else {
          add_property_stringl(return_value, "message", message_str, message_len);
        }
        break;
      case GRPC_OP_RECV_STATUS_ON_CLIENT:
        object_init(&recv_status);
        grpc_parse_metadata_array(&recv_trailing_metadata, array);
        add_property_zval(&recv_status, "metadata", array);
        //TODO(thinkerou): right?
        /*if (Z_REFCOUNT_P(array) == 1) {
          ZVAL_UNREF(array);
        } else {
          Z_DELREF_P(array);
        }*/
        add_property_long(&recv_status, "code", status);
        add_property_string(&recv_status, "details", status_details);
        add_property_zval(return_value, "status", &recv_status);
        if (Z_REFCOUNTED(recv_status)) Z_DELREF(recv_status);
        break;
      case GRPC_OP_RECV_CLOSE_ON_SERVER:
        add_property_bool(return_value, "cancelled", cancelled);
        break;
      default:
        break;
    }
  }

cleanup:
  grpc_metadata_array_destroy(&metadata);
  grpc_metadata_array_destroy(&trailing_metadata);
  grpc_metadata_array_destroy(&recv_metadata);
  grpc_metadata_array_destroy(&recv_trailing_metadata);
  if (status_details != NULL) {
    gpr_free(status_details);
  }
  for (int i = 0; i < op_num; i++) {
    if (ops[i].op == GRPC_OP_SEND_MESSAGE) {
      grpc_byte_buffer_destroy(ops[i].data.send_message);
    }
    if (ops[i].op == GRPC_OP_RECV_MESSAGE) {
      grpc_byte_buffer_destroy(message);
    }
  }
  RETURN_DESTROY_ZVAL(return_value);
}

/**
 * Get the endpoint this call/stream is connected to
 * @return string The URI of the endpoint
 */
PHP_METHOD(Call, getPeer) {
  wrapped_grpc_call *call = Z_WRAPPED_GRPC_CALL_P(getThis());
  RETURN_STRING(grpc_call_get_peer(call->wrapped));
}

/**
 * Cancel the call. This will cause the call to end with STATUS_CANCELLED if it
 * has not already ended with another status.
 */
PHP_METHOD(Call, cancel) {
  wrapped_grpc_call *call = Z_WRAPPED_GRPC_CALL_P(getThis());
  grpc_call_cancel(call->wrapped, NULL);
}

/**
 * Set the CallCredentials for this call.
 * @param CallCredentials creds_obj The CallCredentials object
 * @param int The error code
 */
PHP_METHOD(Call, setCredentials) {
  zval *creds_obj;

  /* "O" == 1 Object */
#ifndef FAST_ZPP
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "O", &creds_obj,
                            grpc_ce_call_credentials) == FAILURE) {
    zend_throw_exception(spl_ce_InvalidArgumentException,
                         "setCredentials expects 1 CallCredentials", 1);
    return;
  }
#else
  ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_OBJECT_OF_CLASS(creds_obj, grpc_ce_call_credentials)
  ZEND_PARSE_PARAMETERS_END();
#endif

  wrapped_grpc_call_credentials *creds = Z_WRAPPED_GRPC_CALL_CREDS_P(creds_obj);
  wrapped_grpc_call *call = Z_WRAPPED_GRPC_CALL_P(getThis());

  grpc_call_error error = GRPC_CALL_ERROR;
  error = grpc_call_set_credentials(call->wrapped, creds->wrapped);
  RETURN_LONG(error);
}

static zend_function_entry call_methods[] = {
    PHP_ME(Call, __construct, NULL, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
    PHP_ME(Call, startBatch, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(Call, getPeer, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(Call, cancel, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(Call, setCredentials, NULL, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

void grpc_init_call() {
  zend_class_entry ce;
  INIT_CLASS_ENTRY(ce, "Grpc\\Call", call_methods);
  ce.create_object = create_wrapped_grpc_call;
  grpc_ce_call = zend_register_internal_class(&ce);
  memcpy(&call_object_handlers_call, zend_get_std_object_handlers(),
         sizeof(zend_object_handlers));
  call_object_handlers_call.offset = XtOffsetOf(wrapped_grpc_call, std);
  call_object_handlers_call.free_obj = free_wrapped_grpc_call;
}
