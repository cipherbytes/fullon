#include <algorithm>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/controller.hpp>
#include <eosio/chain/transaction_context.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/wasm_interface.hpp>
#include <eosio/chain/generated_transaction_object.hpp>
#include <eosio/chain/authorization_manager.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/code_object.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/deep_mind.hpp>
#include <boost/container/flat_set.hpp>
#include <eosio/chain/database_manager.hpp>
#include <eosio/chain/contract_table_context.hpp>
#include <eosio/chain/contract_table_objects.hpp>

using boost::container::flat_set;

namespace eosio { namespace chain {

static inline void print_debug(account_name receiver, const action_trace& ar) {
   if (!ar.console.empty()) {
      auto prefix = fc::format_string(
                                      "\n[(${a},${n})->${r}]",
                                      fc::mutable_variant_object()
                                      ("a", ar.act.account)
                                      ("n", ar.act.name)
                                      ("r", receiver));
      dlog(prefix + ": CONSOLE OUTPUT BEGIN =====================\n"
           + ar.console
           + prefix + ": CONSOLE OUTPUT END   =====================" );
   }
}

apply_context::apply_context(controller& con, transaction_context& trx_ctx, uint32_t action_ordinal, chainbase::database& db, chainbase::database& shared_db,uint32_t depth)
:control(con)
,db(db)
,trx_context(trx_ctx)
,shard_name(trx_ctx.shard_name)
,shared_db(shared_db)
,recurse_depth(depth)
,first_receiver_action_ordinal(action_ordinal)
,action_ordinal(action_ordinal)
,_contract_table(new contract_table_context(*this))
,_contract_shared_table(new contract_shared_table_context(*this))
{
   action_trace& trace = trx_ctx.get_action_trace(action_ordinal);
   act = &trace.act;
   receiver = trace.receiver;
   context_free = trace.context_free;
}

apply_context::~apply_context(){}

void apply_context::exec_one()
{
   auto start = fc::time_point::now();

   digest_type act_digest;
   const account_object*          receiver_account  = nullptr;
   const account_metadata_object* receiver_metadata = nullptr;

   auto handle_exception = [&](const auto& e)
   {
      action_trace& trace = trx_context.get_action_trace( action_ordinal );
      trace.error_code = controller::convert_exception_to_error_code( e );
      trace.except = e;
      finalize_trace( trace, start );
      throw;
   };

   try {
      try {
         action_return_value.clear();
         receiver_account  = &shared_db.get<account_object,by_name>( receiver );

         receiver_metadata = &get_account_metadata( receiver );

         if( !(context_free && control.skip_trx_checks()) ) {
            privileged = receiver_account->is_privileged();
            auto native = control.find_apply_handler( receiver, act->account, act->name );
            if( native ) {
               if( trx_context.enforce_whiteblacklist && control.is_speculative_block() ) {
                  control.check_contract_list( receiver );
                  control.check_action_list( act->account, act->name );
               }
               (*native)( *this );
            }

            if( ( receiver_account->code_hash != digest_type() ) &&
                  (  !( act->account == config::system_account_name
                        && act->name == "setcode"_n
                        && receiver == config::system_account_name )
                     || control.is_builtin_activated( builtin_protocol_feature_t::forward_setcode)
                  )
            ) {
               if( trx_context.enforce_whiteblacklist && control.is_speculative_block() ) {
                  control.check_contract_list( receiver );
                  control.check_action_list( act->account, act->name );
               }
               try {
                  control.get_wasm_interface().apply( receiver_account->code_hash, receiver_account->vm_type, receiver_account->vm_version, *this );
               } catch( const wasm_exit& ) {}
            }

            if( !privileged && control.is_builtin_activated( builtin_protocol_feature_t::ram_restrictions ) ) {
               const size_t checktime_interval = 10;
               size_t counter = 0;
               bool not_in_notify_context = (receiver == act->account);
               const auto end = _account_ram_deltas.end();
               for( auto itr = _account_ram_deltas.begin(); itr != end; ++itr, ++counter ) {
                  if( counter == checktime_interval ) {
                     trx_context.checktime();
                     counter = 0;
                  }
                  if( itr->delta > 0 && itr->account != receiver ) {
                     EOS_ASSERT( not_in_notify_context, unauthorized_ram_usage_increase,
                                 "unprivileged contract cannot increase RAM usage of another account within a notify context: ${account}",
                                 ("account", itr->account)
                     );
                     EOS_ASSERT( has_authorization( itr->account ), unauthorized_ram_usage_increase,
                                 "unprivileged contract cannot increase RAM usage of another account that has not authorized the action: ${account}",
                                 ("account", itr->account)
                     );
                  }
               }
            }
         }
      } FC_RETHROW_EXCEPTIONS( warn, "pending console output: ${console}", ("console", _pending_console_output) )

      if( control.is_builtin_activated( builtin_protocol_feature_t::action_return_value ) ) {
         act_digest =   generate_action_digest(
                           [this](const char* data, uint32_t datalen) {
                              return trx_context.hash_with_checktime<digest_type>(data, datalen);
                           },
                           *act,
                           action_return_value
                        );
      } else {
         act_digest = digest_type::hash(*act);
      }
   } catch ( const std::bad_alloc& ) {
      throw;
   } catch ( const boost::interprocess::bad_alloc& ) {
      throw;
   } catch( const fc::exception& e ) {
      handle_exception(e);
   } catch ( const std::exception& e ) {
      auto wrapper = fc::std_exception_wrapper::from_current_exception(e);
      handle_exception(wrapper);
   }

   // Note: It should not be possible for receiver_account to be invalidated because:
   //    * a pointer to an object in a chainbase index is not invalidated if other objects in that index are modified, removed, or added;
   //    * a pointer to an object in a chainbase index is not invalidated if the fields of that object are modified;
   //    * and, the *receiver_account object itself cannot be removed because accounts cannot be deleted in EOSIO.

   action_trace& trace = trx_context.get_action_trace( action_ordinal );
   trace.return_value  = std::move(action_return_value);
   trace.receipt.emplace();

   action_receipt& r  = *trace.receipt;
   r.receiver         = receiver;
   r.act_digest       = act_digest;
   r.global_sequence  = next_global_sequence();
   r.recv_sequence    = next_recv_sequence( *receiver_metadata );

   const account_object* first_receiver_account = nullptr;
   if( act->account == receiver ) {
      first_receiver_account = receiver_account;
   } else {
      first_receiver_account = &shared_db.get<account_object, by_name>(act->account);
   }

   r.code_sequence    = first_receiver_account->code_sequence; // could be modified by action execution above
   r.abi_sequence     = first_receiver_account->abi_sequence;  // could be modified by action execution above

   for( const auto& auth : act->authorization ) {
      r.auth_sequence[auth.actor] = next_auth_sequence( auth.actor );
   }

   trx_context.executed_action_receipt_digests.emplace_back( r.digest() );

   finalize_trace( trace, start );

   if ( control.contracts_console() ) {
      print_debug(receiver, trace);
   }

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient()))
   {
      dm_logger->on_end_action();
   }
}

void apply_context::finalize_trace( action_trace& trace, const fc::time_point& start )
{
   trace.account_ram_deltas = std::move( _account_ram_deltas );
   _account_ram_deltas.clear();

   trace.console = std::move( _pending_console_output );
   _pending_console_output.clear();

   trace.elapsed = fc::time_point::now() - start;
}

void apply_context::exec()
{
   _notified.emplace_back( receiver, action_ordinal );
   exec_one();
   for( uint32_t i = 1; i < _notified.size(); ++i ) {
      std::tie( receiver, action_ordinal ) = _notified[i];
      exec_one();
   }

   if( _cfa_inline_actions.size() > 0 || _inline_actions.size() > 0 ) {
      EOS_ASSERT( recurse_depth < control.get_global_properties().configuration.max_inline_action_depth,
                  transaction_exception, "max inline action depth per transaction reached" );
   }

   for( uint32_t ordinal : _cfa_inline_actions ) {
      trx_context.execute_action( ordinal, recurse_depth + 1 );
   }

   for( uint32_t ordinal : _inline_actions ) {
      trx_context.execute_action( ordinal, recurse_depth + 1 );
   }

} /// exec()

bool apply_context::is_account( const account_name& account )const {
   return nullptr != shared_db.find<account_object,by_name>( account );
}

void apply_context::get_code_hash(
   account_name account, uint64_t& code_sequence, fc::sha256& code_hash, uint8_t& vm_type, uint8_t& vm_version) const {
   auto obj = shared_db.find<account_object,by_name>(account);
   if(!obj || obj->code_hash == fc::sha256{}) {
      if(obj)
         code_sequence = obj->code_sequence;
      else
         code_sequence = 0;
      code_hash = {};
      vm_type = 0;
      vm_version = 0;
   } else {
      code_sequence = obj->code_sequence;
      code_hash = obj->code_hash;
      vm_type = obj->vm_type;
      vm_version = obj->vm_version;
   }
}

void apply_context::require_authorization( const account_name& account ) {
   for( uint32_t i=0; i < act->authorization.size(); i++ ) {
     if( act->authorization[i].actor == account ) {
        return;
     }
   }
   EOS_ASSERT( false, missing_auth_exception, "missing authority of ${account}", ("account",account));
}

bool apply_context::has_authorization( const account_name& account )const {
   for( const auto& auth : act->authorization )
     if( auth.actor == account )
        return true;
  return false;
}

void apply_context::require_authorization(const account_name& account,
                                          const permission_name& permission) {
  for( uint32_t i=0; i < act->authorization.size(); i++ )
     if( act->authorization[i].actor == account ) {
        if( act->authorization[i].permission == permission ) {
           return;
        }
     }
  EOS_ASSERT( false, missing_auth_exception, "missing authority of ${account}/${permission}",
              ("account",account)("permission",permission) );
}

bool apply_context::has_recipient( account_name code )const {
   for( const auto& p : _notified )
      if( p.first == code )
         return true;
   return false;
}

void apply_context::require_recipient( account_name recipient ) {
   if( !has_recipient(recipient) ) {
      _notified.emplace_back(
         recipient,
         schedule_action( action_ordinal, recipient, false )
      );

      if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
         dm_logger->on_require_recipient();
      }
   }
}


/**
 *  This will execute an action after checking the authorization. Inline transactions are
 *  implicitly authorized by the current receiver (running code). This method has significant
 *  security considerations and several options have been considered:
 *
 *  1. privileged accounts (those marked as such by block producers) can authorize any action
 *  2. all other actions are only authorized by 'receiver' which means the following:
 *         a. the user must set permissions on their account to allow the 'receiver' to act on their behalf
 *
 *  Discarded Implementation: at one point we allowed any account that authorized the current transaction
 *   to implicitly authorize an inline transaction. This approach would allow privilege escalation and
 *   make it unsafe for users to interact with certain contracts.  We opted instead to have applications
 *   ask the user for permission to take certain actions rather than making it implicit. This way users
 *   can better understand the security risk.
 */
void apply_context::execute_inline( action&& a ) {
   auto* code = shared_db.find<account_object, by_name>(a.account);
   EOS_ASSERT( code != nullptr, action_validate_exception,
               "inline action's code account ${account} does not exist", ("account", a.account) );

   bool enforce_actor_whitelist_blacklist = trx_context.enforce_whiteblacklist && control.is_speculative_block();
   flat_set<account_name> actors;

   bool disallow_send_to_self_bypass = control.is_builtin_activated( builtin_protocol_feature_t::restrict_action_to_self );
   bool send_to_self = (a.account == receiver);
   bool inherit_parent_authorizations = (!disallow_send_to_self_bypass && send_to_self && (receiver == act->account) && control.is_speculative_block());

   flat_set<permission_level> inherited_authorizations;
   if( inherit_parent_authorizations ) {
      inherited_authorizations.reserve( a.authorization.size() );
   }

   for( const auto& auth : a.authorization ) {
      auto* actor = shared_db.find<account_object, by_name>(auth.actor);
      EOS_ASSERT( actor != nullptr, action_validate_exception,
                  "inline action's authorizing actor ${account} does not exist", ("account", auth.actor) );
      EOS_ASSERT( control.get_authorization_manager().find_permission(auth) != nullptr, action_validate_exception,
                  "inline action's authorizations include a non-existent permission: ${permission}",
                  ("permission", auth) );
      if( enforce_actor_whitelist_blacklist )
         actors.insert( auth.actor );

      if( inherit_parent_authorizations && std::find(act->authorization.begin(), act->authorization.end(), auth) != act->authorization.end() ) {
         inherited_authorizations.insert( auth );
      }
   }

   if( enforce_actor_whitelist_blacklist ) {
      control.check_actor_list( actors );
   }

   if( !privileged && control.is_speculative_block() ) {
      const auto& chain_config = control.get_global_properties().configuration;
      EOS_ASSERT( a.data.size() < std::min(chain_config.max_inline_action_size, control.get_max_nonprivileged_inline_action_size()),
                  inline_action_too_big_nonprivileged,
                  "inline action too big for nonprivileged account ${account}", ("account", a.account));
   }
   // No need to check authorization if replaying irreversible blocks or contract is privileged
   if( !control.skip_auth_check() && !privileged && !trx_context.is_read_only() ) {
      try {
         control.get_authorization_manager()
                .check_authorization( {a},
                                      {},
                                      {{receiver, config::eosio_code_name}},
                                      control.pending_block_time() - trx_context.published,
                                      std::bind(&transaction_context::checktime, &this->trx_context),
                                      false,
                                      trx_context.is_dry_run(), // check_but_dont_fail
                                      inherited_authorizations
                                    );

         //QUESTION: Is it smart to allow a deferred transaction that has been delayed for some time to get away
         //          with sending an inline action that requires a delay even though the decision to send that inline
         //          action was made at the moment the deferred transaction was executed with potentially no forewarning?
      } catch( const fc::exception& e ) {
         if( disallow_send_to_self_bypass || !send_to_self ) {
            throw;
         } else if( control.is_speculative_block() ) {
            subjective_block_production_exception new_exception(FC_LOG_MESSAGE( error, "Authorization failure with inline action sent to self"));
            for (const auto& log: e.get_log()) {
               new_exception.append_log(log);
            }
            throw new_exception;
         }
      } catch( ... ) {
         if( disallow_send_to_self_bypass || !send_to_self ) {
            throw;
         } else if( control.is_speculative_block() ) {
            EOS_THROW(subjective_block_production_exception, "Unexpected exception occurred validating inline action sent to self");
         }
      }
   }

   auto inline_receiver = a.account;
   _inline_actions.emplace_back(
      schedule_action( std::move(a), inline_receiver, false )
   );

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      dm_logger->on_send_inline();
   }
}

void apply_context::execute_context_free_inline( action&& a ) {
   auto* code = db.find<account_object, by_name>(a.account);
   EOS_ASSERT( code != nullptr, action_validate_exception,
               "inline action's code account ${account} does not exist", ("account", a.account) );

   EOS_ASSERT( a.authorization.size() == 0, action_validate_exception,
               "context-free actions cannot have authorizations" );

   if( !privileged && control.is_speculative_block() ) {
      const auto& chain_config = control.get_global_properties().configuration;
      EOS_ASSERT( a.data.size() < std::min(chain_config.max_inline_action_size, control.get_max_nonprivileged_inline_action_size()),
                  inline_action_too_big_nonprivileged,
                  "inline action too big for nonprivileged account ${account}", ("account", a.account));
   }

   auto inline_receiver = a.account;
   _cfa_inline_actions.emplace_back(
      schedule_action( std::move(a), inline_receiver, true )
   );

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      dm_logger->on_send_context_free_inline();
   }
}


void apply_context::schedule_deferred_transaction( const uint128_t& sender_id, account_name payer, transaction&& trx, bool replace_existing ) {
   EOS_ASSERT( !trx_context.is_read_only(), transaction_exception, "cannot schedule a deferred transaction from within a readonly transaction" );
   EOS_ASSERT( trx.context_free_actions.size() == 0, cfa_inside_generated_tx, "context free actions are not currently allowed in generated transactions" );

   assert(shard_name == config::main_shard_name);
   bool enforce_actor_whitelist_blacklist = trx_context.enforce_whiteblacklist && control.is_speculative_block()
                                             && !control.sender_avoids_whitelist_blacklist_enforcement( receiver );
   trx_context.validate_referenced_accounts( trx, enforce_actor_whitelist_blacklist );

   if( control.is_builtin_activated( builtin_protocol_feature_t::no_duplicate_deferred_id ) ) {
      auto exts = trx.validate_and_extract_extensions();
      if( exts.size() > 0 ) {
         auto itr = exts.lower_bound( deferred_transaction_generation_context::extension_id() );

         EOS_ASSERT( exts.size() == 1 && itr != exts.end(), invalid_transaction_extension,
                     "only the deferred_transaction_generation_context extension is currently supported for deferred transactions"
         );

         const auto& context = std::get<deferred_transaction_generation_context>(itr->second);

         EOS_ASSERT( context.sender == receiver, ill_formed_deferred_transaction_generation_context,
                     "deferred transaction generaction context contains mismatching sender",
                     ("expected", receiver)("actual", context.sender)
         );
         EOS_ASSERT( context.sender_id == sender_id, ill_formed_deferred_transaction_generation_context,
                     "deferred transaction generaction context contains mismatching sender_id",
                     ("expected", sender_id)("actual", context.sender_id)
         );
         EOS_ASSERT( context.sender_trx_id == trx_context.packed_trx.id(), ill_formed_deferred_transaction_generation_context,
                     "deferred transaction generaction context contains mismatching sender_trx_id",
                     ("expected", trx_context.packed_trx.id())("actual", context.sender_trx_id)
         );
      } else {
         emplace_extension(
            trx.transaction_extensions,
            deferred_transaction_generation_context::extension_id(),
            fc::raw::pack( deferred_transaction_generation_context( trx_context.packed_trx.id(), sender_id, receiver ) )
         );
      }
      trx.expiration = time_point_sec();
      trx.ref_block_num = 0;
      trx.ref_block_prefix = 0;
   } else {
      trx.expiration = control.pending_block_time() + fc::microseconds(999'999); // Rounds up to nearest second (makes expiration check unnecessary)
      trx.set_reference_block(control.head_block_id()); // No TaPoS check necessary
   }

   // Charge ahead of time for the additional net usage needed to retire the deferred transaction
   // whether that be by successfully executing, soft failure, hard failure, or expiration.
   const auto& cfg = control.get_global_properties().configuration;
   trx_context.add_net_usage( static_cast<uint64_t>(cfg.base_per_transaction_net_usage)
                               + static_cast<uint64_t>(config::transaction_id_net_usage) ); // Will exit early if net usage cannot be payed.

   auto delay = fc::seconds(trx.delay_sec);

   bool ram_restrictions_activated = control.is_builtin_activated( builtin_protocol_feature_t::ram_restrictions );

   if( !control.skip_auth_check() && !privileged ) { // Do not need to check authorization if replayng irreversible block or if contract is privileged
      if( payer != receiver ) {
         if( ram_restrictions_activated ) {
            EOS_ASSERT( receiver == act->account, action_validate_exception,
                        "cannot bill RAM usage of deferred transactions to another account within notify context"
            );
            EOS_ASSERT( has_authorization( payer ), action_validate_exception,
                        "cannot bill RAM usage of deferred transaction to another account that has not authorized the action: ${payer}",
                        ("payer", payer)
            );
         } else {
            require_authorization(payer); /// uses payer's storage
         }
      }

      // Originally this code bypassed authorization checks if a contract was deferring only actions to itself.
      // The idea was that the code could already do whatever the deferred transaction could do, so there was no point in checking authorizations.
      // But this is not true. The original implementation didn't validate the authorizations on the actions which allowed for privilege escalation.
      // It would make it possible to bill RAM to some unrelated account.
      // Furthermore, even if the authorizations were forced to be a subset of the current action's authorizations, it would still violate the expectations
      // of the signers of the original transaction, because the deferred transaction would allow billing more CPU and network bandwidth than the maximum limit
      // specified on the original transaction.
      // So, the deferred transaction must always go through the authorization checking if it is not sent by a privileged contract.
      // However, the old logic must still be considered because it cannot objectively change until a consensus protocol upgrade.

      bool disallow_send_to_self_bypass = control.is_builtin_activated( builtin_protocol_feature_t::restrict_action_to_self );

      auto is_sending_only_to_self = [&trx]( const account_name& self ) {
         bool send_to_self = true;
         for( const auto& act : trx.actions ) {
            if( act.account != self ) {
               send_to_self = false;
               break;
            }
         }
         return send_to_self;
      };

      try {
         control.get_authorization_manager()
                .check_authorization( trx.actions,
                                      {},
                                      {{receiver, config::eosio_code_name}},
                                      delay,
                                      std::bind(&transaction_context::checktime, &this->trx_context),
                                      false
                                    );
      } catch( const fc::exception& e ) {
         if( disallow_send_to_self_bypass || !is_sending_only_to_self(receiver) ) {
            throw;
         } else if( control.is_speculative_block() ) {
            subjective_block_production_exception new_exception(FC_LOG_MESSAGE( error, "Authorization failure with sent deferred transaction consisting only of actions to self"));
            for (const auto& log: e.get_log()) {
               new_exception.append_log(log);
            }
            throw new_exception;
         }
      } catch( ... ) {
         if( disallow_send_to_self_bypass || !is_sending_only_to_self(receiver) ) {
            throw;
         } else if( control.is_speculative_block() ) {
            EOS_THROW(subjective_block_production_exception, "Unexpected exception occurred validating sent deferred transaction consisting only of actions to self");
         }
      }
   }

   uint32_t trx_size = 0;
   if ( auto ptr = db.find<generated_transaction_object,by_sender_id>(boost::make_tuple(receiver, sender_id)) ) {
      EOS_ASSERT( replace_existing, deferred_tx_duplicate, "deferred transaction with the same sender_id and payer already exists" );

      bool replace_deferred_activated = control.is_builtin_activated(builtin_protocol_feature_t::replace_deferred);

      EOS_ASSERT( replace_deferred_activated || !control.is_speculative_block()
                     || control.all_subjective_mitigations_disabled(),
                  subjective_block_production_exception,
                  "Replacing a deferred transaction is temporarily disabled." );

      if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
         dm_logger->on_ram_trace(RAM_EVENT_ID("${id}", ("id", ptr->id)), "deferred_trx", "cancel", "deferred_trx_cancel");
      }

      uint64_t orig_trx_ram_bytes = config::billable_size_v<generated_transaction_object> + ptr->packed_trx.size();
      if( replace_deferred_activated ) {
         add_ram_usage( ptr->payer, -static_cast<int64_t>( orig_trx_ram_bytes ) );
      } else {
         control.add_to_ram_correction( ptr->payer, orig_trx_ram_bytes );
      }

      transaction_id_type trx_id_for_new_obj;
      if( replace_deferred_activated ) {
         trx_id_for_new_obj = trx.id();
      } else {
         trx_id_for_new_obj = ptr->trx_id;
      }

      if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
         dm_logger->on_cancel_deferred(deep_mind_handler::operation_qualifier::modify, *ptr);
      }

      // Use remove and create rather than modify because mutating the trx_id field in a modifier is unsafe.
      db.remove( *ptr );
      db.create<generated_transaction_object>( [&]( auto& gtx ) {
         gtx.trx_id      = trx_id_for_new_obj;
         gtx.sender      = receiver;
         gtx.sender_id   = sender_id;
         gtx.payer       = payer;
         gtx.published   = control.pending_block_time();
         gtx.delay_until = gtx.published + delay;
         gtx.expiration  = gtx.delay_until + fc::seconds(control.get_global_properties().configuration.deferred_trx_expiration_window);

         trx_size = gtx.set( trx );
         gtx.shard_name = shard_name;

         if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
            dm_logger->on_send_deferred(deep_mind_handler::operation_qualifier::modify, gtx);
            dm_logger->on_ram_trace(RAM_EVENT_ID("${id}", ("id", gtx.id)), "deferred_trx", "update", "deferred_trx_add");
         }
      } );
   } else {
      db.create<generated_transaction_object>( [&]( auto& gtx ) {
         gtx.trx_id      = trx.id();
         gtx.sender      = receiver;
         gtx.sender_id   = sender_id;
         gtx.payer       = payer;
         gtx.published   = control.pending_block_time();
         gtx.delay_until = gtx.published + delay;
         gtx.expiration  = gtx.delay_until + fc::seconds(control.get_global_properties().configuration.deferred_trx_expiration_window);

         trx_size = gtx.set( trx );
         gtx.shard_name = shard_name;

         if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
            dm_logger->on_send_deferred(deep_mind_handler::operation_qualifier::none, gtx);
            dm_logger->on_ram_trace(RAM_EVENT_ID("${id}", ("id", gtx.id)), "deferred_trx", "add", "deferred_trx_add");
         }
      } );
   }

   EOS_ASSERT( ram_restrictions_activated
               || control.is_ram_billing_in_notify_allowed()
               || (receiver == act->account) || (receiver == payer) || privileged,
               subjective_block_production_exception,
               "Cannot charge RAM to other accounts during notify."
   );
   add_ram_usage( payer, (config::billable_size_v<generated_transaction_object> + trx_size) );
}

bool apply_context::cancel_deferred_transaction( const uint128_t& sender_id, account_name sender ) {
   EOS_ASSERT( !trx_context.is_read_only(), transaction_exception, "cannot cancel a deferred transaction from within a readonly transaction" );
   auto& generated_transaction_idx = db.get_mutable_index<generated_transaction_multi_index>();
   const auto* gto = db.find<generated_transaction_object,by_sender_id>(boost::make_tuple(sender, sender_id));
   if ( gto ) {
      if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
         dm_logger->on_cancel_deferred(deep_mind_handler::operation_qualifier::none, *gto);
         dm_logger->on_ram_trace(RAM_EVENT_ID("${id}", ("id", gto->id)), "deferred_trx", "cancel", "deferred_trx_cancel");
      }

      add_ram_usage( gto->payer, -(config::billable_size_v<generated_transaction_object> + gto->packed_trx.size()) );
      generated_transaction_idx.remove(*gto);
   }
   return gto;
}

uint32_t apply_context::schedule_action( uint32_t ordinal_of_action_to_schedule, account_name receiver, bool context_free )
{
   uint32_t scheduled_action_ordinal = trx_context.schedule_action( ordinal_of_action_to_schedule,
                                                                    receiver, context_free,
                                                                    action_ordinal, first_receiver_action_ordinal );

   act = &trx_context.get_action_trace( action_ordinal ).act;
   return scheduled_action_ordinal;
}

uint32_t apply_context::schedule_action( action&& act_to_schedule, account_name receiver, bool context_free )
{
   uint32_t scheduled_action_ordinal = trx_context.schedule_action( std::move(act_to_schedule),
                                                                    receiver, context_free,
                                                                    action_ordinal, first_receiver_action_ordinal );

   act = &trx_context.get_action_trace( action_ordinal ).act;
   return scheduled_action_ordinal;
}

vector<account_name> apply_context::get_active_producers() const {
   const auto& ap = control.active_producers();
   vector<account_name> accounts; accounts.reserve( ap.producers.size() );

   for(const auto& producer : ap.producers )
      accounts.push_back(producer.producer_name);

   return accounts;
}

void apply_context::update_db_usage( const account_name& payer, int64_t delta ) {
   if( delta > 0 ) {
      if( !(privileged || payer == account_name(receiver)
               || control.is_builtin_activated( builtin_protocol_feature_t::ram_restrictions ) ) )
      {
         EOS_ASSERT( control.is_ram_billing_in_notify_allowed() || (receiver == act->account),
                     subjective_block_production_exception, "Cannot charge RAM to other accounts during notify." );
         require_authorization( payer );
      }
   }
   add_ram_usage(payer, delta);
}


int apply_context::get_action( uint32_t type, uint32_t index, char* buffer, size_t buffer_size )const
{
   const auto& trx = trx_context.packed_trx.get_transaction();
   const action* act_ptr = nullptr;

   if( type == 0 ) {
      if( index >= trx.context_free_actions.size() )
         return -1;
      act_ptr = &trx.context_free_actions[index];
   }
   else if( type == 1 ) {
      if( index >= trx.actions.size() )
         return -1;
      act_ptr = &trx.actions[index];
   }

   EOS_ASSERT(act_ptr, action_not_found_exception, "action is not found" );

   auto ps = fc::raw::pack_size( *act_ptr );
   if( ps <= buffer_size ) {
      fc::datastream<char*> ds(buffer, buffer_size);
      fc::raw::pack( ds, *act_ptr );
   }
   return ps;
}

int apply_context::get_context_free_data( uint32_t index, char* buffer, size_t buffer_size )const
{
   const auto& trx = trx_context.packed_trx.get_signed_transaction();

   if( index >= trx.context_free_data.size() ) return -1;

   auto s = trx.context_free_data[index].size();
   if( buffer_size == 0 ) return s;

   auto copy_size = std::min( buffer_size, s );
   memcpy( buffer, trx.context_free_data[index].data(), copy_size );

   return copy_size;
}

uint64_t apply_context::next_global_sequence() {
   if ( trx_context.is_read_only() ) {
      // To avoid confusion of duplicated global sequence number, hard code to be 0.
      return 0;
   } else {
      const dynamic_global_property_object* p = db.find<dynamic_global_property_object>();
      //The object here may not have been created yet.
      if( p == nullptr ) {
         p = &db.create<dynamic_global_property_object>([&](auto& d) {
            ++d.global_action_sequence;
         });
      } else {
         db.modify( *p, [&]( auto& dgp ) {
            ++dgp.global_action_sequence;
         });
      }
      return p->global_action_sequence;
   }
}

uint64_t apply_context::next_recv_sequence( const account_metadata_object& receiver_account ) {
   if ( trx_context.is_read_only() ) {
      // To avoid confusion of duplicated receive sequence number, hard code to be 0.
      return 0;
   } else {
      db.modify( receiver_account, [&]( auto& ra ) {
         ++ra.recv_sequence;
      });
      return receiver_account.recv_sequence;
   }
}
uint64_t apply_context::next_auth_sequence( account_name actor ) {
   const auto& amo = get_account_metadata( actor );
   db.modify( amo, [&](auto& am ){
      ++am.auth_sequence;
   });
   return amo.auth_sequence;
}

void apply_context::add_ram_usage( account_name account, int64_t ram_delta ) {
   trx_context.add_ram_usage( account, ram_delta );

   auto p = _account_ram_deltas.emplace( account, ram_delta );
   if( !p.second ) {
      p.first->delta += ram_delta;
   }
}

action_name apply_context::get_sender() const {
   const action_trace& trace = trx_context.get_action_trace( action_ordinal );
   if (trace.creator_action_ordinal > 0) {
      const action_trace& creator_trace = trx_context.get_action_trace( trace.creator_action_ordinal );
      return creator_trace.receiver;
   }
   return action_name();
}

contract_table_context& apply_context::table_context() {
   return *_contract_table;
}

contract_shared_table_context& apply_context::shared_table_context() {
   return *_contract_shared_table;
}

bool apply_context::is_builtin_activated( builtin_protocol_feature_t f ) const {
   return control.is_builtin_activated( f );
}

bool apply_context::is_speculative_block() const {
   return control.is_speculative_block();
}


const account_metadata_object& apply_context::get_account_metadata(const name& account) {
   auto ret = db.find<account_metadata_object, by_name>( account );
   if( ret != nullptr )
      return *ret;
   // account_metadata_object may not be initialized in sub-shard when account first access
   return db.create<account_metadata_object>([&](auto& a) {
      a.name = account;
   });
}

} } /// eosio::chain
