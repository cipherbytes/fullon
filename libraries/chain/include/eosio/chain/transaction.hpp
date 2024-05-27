#pragma once

#include <eosio/chain/action.hpp>
#include <eosio/chain/types.hpp>
#include <numeric>

namespace eosio { namespace chain {

   struct deferred_transaction_generation_context : fc::reflect_init {
      static constexpr uint16_t extension_id() { return 0; }
      static constexpr bool     enforce_unique() { return true; }

      deferred_transaction_generation_context() = default;

      deferred_transaction_generation_context( const transaction_id_type& sender_trx_id, uint128_t sender_id, account_name sender )
      :sender_trx_id( sender_trx_id )
      ,sender_id( sender_id )
      ,sender( sender )
      {}

      void reflector_init();

      transaction_id_type sender_trx_id;
      uint128_t           sender_id;
      account_name        sender;
   };

   enum class shard_type: uint8_t {
      normal      = 0,
      privacy     = 1
   };

   using shard_type_enum = fc::enum_type<uint8_t, shard_type>;

   struct transaction_shard : fc::reflect_init {
      static constexpr uint16_t extension_id() { return 1; }
      static constexpr bool     enforce_unique() { return true; }

      transaction_shard() = default;
      transaction_shard( const chain::shard_name& shard_name, chain::shard_type shard_type)
      :shard_name(shard_name)
      ,shard_type(shard_type){}

      eosio::chain::shard_name   shard_name = default_shard_name; // shard name
      shard_type_enum            shard_type = shard_type_enum(chain::shard_type::normal);
      void reflector_init();

      static chain::shard_name default_shard_name;
   };

   namespace detail {
      template<typename... Ts>
      struct transaction_extension_types {
         using transaction_extension_t = std::variant< Ts... >;
         using decompose_t = decompose< Ts... >;

         inline static std::vector<char> pack_data(const transaction_extension_t& ext) {
            return std::visit([](const auto& t) -> auto { return fc::raw::pack(t); }, ext);
         }
      };
   }

   using transaction_extension_types = detail::transaction_extension_types<
      deferred_transaction_generation_context,
      transaction_shard
   >;

   using transaction_extension = transaction_extension_types::transaction_extension_t;

   /**
    *  The transaction header contains the fixed-sized data
    *  associated with each transaction. It is separated from
    *  the transaction body to facilitate partial parsing of
    *  transactions without requiring dynamic memory allocation.
    *
    *  All transactions have an expiration time after which they
    *  may no longer be included in the blockchain. Once a block
    *  with a block_header::timestamp greater than expiration is
    *  deemed irreversible, then a user can safely trust the transaction
    *  will never be included.
    */

   struct transaction_header : fc::reflect_init {
      time_point_sec             expiration;   ///< the time at which a transaction expires
      uint16_t                   ref_block_num       = 0U; ///< specifies a block num in the last 2^16 blocks.
      uint32_t                   ref_block_prefix    = 0UL; ///< specifies the lower 32 bits of the blockid at get_ref_blocknum
      fc::unsigned_int           max_net_usage_words = 0UL; /// upper limit on total network bandwidth (in 8 byte words) billed for this transaction
      uint8_t                    max_cpu_usage_ms    = 0; /// upper limit on the total CPU time billed for this transaction
      fc::unsigned_int           delay_sec           = 0UL; /// number of seconds to delay this transaction for during which it may be canceled.

      transaction_header() = default;
      transaction_header(  const time_point_sec&      expiration,
                           uint16_t                   ref_block_num,
                           uint32_t                   ref_block_prefix,
                           fc::unsigned_int           max_net_usage_words,
                           uint8_t                    max_cpu_usage_ms,
                           fc::unsigned_int           delay_sec)
                             :expiration(expiration),
                              ref_block_num(ref_block_num),
                              ref_block_prefix(ref_block_prefix),
                              max_net_usage_words(max_net_usage_words),
                              max_cpu_usage_ms(max_cpu_usage_ms),
                              delay_sec(delay_sec) {}

      /**
       * @return the absolute block number given the relative ref_block_num
       */
      block_num_type get_ref_blocknum( block_num_type head_blocknum )const {
         return ((head_blocknum/0xffff)*0xffff) + head_blocknum%0xffff;
      }
      void set_reference_block( const block_id_type& reference_block );
      bool verify_reference_block( const block_id_type& reference_block )const;
      void validate()const;

      void reflector_init() {}
   };

   /**
    *  A transaction consists of a set of messages which must all be applied or
    *  all are rejected. These messages have access to data within the given
    *  read and write scopes.
    */
   struct transaction : public transaction_header {
      vector<action>         context_free_actions;
      vector<action>         actions;
      extensions_type        transaction_extensions;

      using transaction_header::transaction_header;
      transaction_id_type        id()const;
      digest_type                sig_digest( const chain_id_type& chain_id, const vector<bytes>& cfd = vector<bytes>() )const;
      fc::microseconds           get_signature_keys( const vector<signature_type>& signatures,
                                                     const chain_id_type& chain_id,
                                                     fc::time_point deadline,
                                                     const vector<bytes>& cfd,
                                                     flat_set<public_key_type>& recovered_pub_keys,
                                                     bool allow_duplicate_keys = false) const;

      uint32_t total_actions()const { return context_free_actions.size() + actions.size(); }

      account_name first_authorizer()const {
         for( const auto& a : actions ) {
            for( const auto& u : a.authorization )
               return u.actor;
         }
         return account_name();
      }

      typedef flat_multimap<uint16_t, transaction_extension> transaction_extension_map;

      const transaction_extension_map& get_extracted_extensions()const;

      transaction_extension_map::iterator emplace_extension_unique( transaction_extension&& ext);

      void reflector_init();

      const chain::shard_name& get_shard_name() const { return _shard_name; }
      void set_shard(const eosio::chain::shard_name& shard_name, chain::shard_type shard_type = chain::shard_type::normal);
      const eosio::chain::shard_type& get_shard_type() const  { return _shard_type; }
      bool has_shard_extension() const;
   private:
      flat_multimap<uint16_t, transaction_extension>  _extracted_extensions;

      eosio::chain::shard_name   _shard_name          = transaction_shard::default_shard_name; // shard name
      eosio::chain::shard_type   _shard_type          = eosio::chain::shard_type::normal;

      void extract_extensions();
   };

   struct signed_transaction : public transaction
   {
      signed_transaction() = default;
//      signed_transaction( const signed_transaction& ) = default;
//      signed_transaction( signed_transaction&& ) = default;
      signed_transaction( transaction&& trx, const vector<signature_type>& signatures, const vector<bytes>& context_free_data)
      : transaction(std::move(trx))
      , signatures(signatures)
      , context_free_data(context_free_data)
      {}
      signed_transaction( transaction&& trx, const vector<signature_type>& signatures, vector<bytes>&& context_free_data)
      : transaction(std::move(trx))
      , signatures(signatures)
      , context_free_data(std::move(context_free_data))
      {}

      vector<signature_type>    signatures;
      vector<bytes>             context_free_data; ///< for each context-free action, there is an entry here

      const signature_type&     sign(const private_key_type& key, const chain_id_type& chain_id);
      signature_type            sign(const private_key_type& key, const chain_id_type& chain_id)const;
      fc::microseconds          get_signature_keys( const chain_id_type& chain_id, fc::time_point deadline,
                                                    flat_set<public_key_type>& recovered_pub_keys,
                                                    bool allow_duplicate_keys = false )const;
   };

   struct packed_transaction : fc::reflect_init {
      enum class compression_type {
         none = 0,
         zlib = 1,
      };

      packed_transaction() = default;
      packed_transaction(packed_transaction&&) = default;
      explicit packed_transaction(const packed_transaction&) = default;
      packed_transaction& operator=(const packed_transaction&) = delete;
      packed_transaction& operator=(packed_transaction&&) = default;

      explicit packed_transaction(const signed_transaction& t, compression_type _compression = compression_type::none)
      :signatures(t.signatures), compression(_compression), unpacked_trx(t), trx_id(unpacked_trx.id())
      {
         local_pack_transaction();
         local_pack_context_free_data();
      }

      explicit packed_transaction(signed_transaction&& t, compression_type _compression = compression_type::none)
      :signatures(t.signatures), compression(_compression), unpacked_trx(std::move(t)), trx_id(unpacked_trx.id())
      {
         local_pack_transaction();
         local_pack_context_free_data();
      }

      // used by abi_serializer
      packed_transaction( bytes&& packed_txn, vector<signature_type>&& sigs, bytes&& packed_cfd, compression_type _compression );
      packed_transaction( bytes&& packed_txn, vector<signature_type>&& sigs, vector<bytes>&& cfd, compression_type _compression );
      packed_transaction( transaction&& t, vector<signature_type>&& sigs, bytes&& packed_cfd, compression_type _compression );

      friend bool operator==(const packed_transaction& lhs, const packed_transaction& rhs) {
         return std::tie(lhs.signatures, lhs.compression, lhs.packed_context_free_data, lhs.packed_trx) ==
                std::tie(rhs.signatures, rhs.compression, rhs.packed_context_free_data, rhs.packed_trx);
      }
      friend bool operator!=(const packed_transaction& lhs, const packed_transaction& rhs) { return !(lhs == rhs); }

      uint32_t get_unprunable_size()const;
      uint32_t get_prunable_size()const;
      size_t get_estimated_size()const;

      digest_type packed_digest()const;

      const transaction_id_type& id()const { return trx_id; }
      const eosio::chain::shard_name& get_shard_name() const {
         return unpacked_trx.get_shard_name();
      }
      const eosio::chain::shard_type& get_shard_type() const {
         return unpacked_trx.get_shard_type();
      }
      bytes               get_raw_transaction()const;

      time_point_sec                expiration()const { return unpacked_trx.expiration; }
      const vector<bytes>&          get_context_free_data()const { return unpacked_trx.context_free_data; }
      const transaction&            get_transaction()const { return unpacked_trx; }
      const signed_transaction&     get_signed_transaction()const { return unpacked_trx; }
      const vector<signature_type>& get_signatures()const { return signatures; }
      const fc::enum_type<uint8_t,compression_type>& get_compression()const { return compression; }
      const bytes&                  get_packed_context_free_data()const { return packed_context_free_data; }
      const bytes&                  get_packed_transaction()const { return packed_trx; }

   private:
      void local_unpack_transaction(vector<bytes>&& context_free_data);
      void local_unpack_context_free_data();
      void local_pack_transaction();
      void local_pack_context_free_data();

      friend struct fc::reflector<packed_transaction>;
      friend struct fc::reflector_init_visitor<packed_transaction>;
      friend struct fc::has_reflector_init<packed_transaction>;
      void reflector_init();
   private:
      vector<signature_type>                  signatures;
      fc::enum_type<uint8_t,compression_type> compression;
      bytes                                   packed_context_free_data;
      // transaction (not signed_transaction) packed and possibly compressed according to compression
      bytes                                   packed_trx;

   private:
      // cache unpacked trx, for thread safety do not modify after construction
      signed_transaction                      unpacked_trx;
      transaction_id_type                     trx_id;
   };

   using packed_transaction_ptr = std::shared_ptr<const packed_transaction>;

   uint128_t transaction_id_to_sender_id( const transaction_id_type& tid );

} } /// namespace eosio::chain

FC_REFLECT_ENUM( eosio::chain::shard_type, (normal)(privacy))
FC_REFLECT(eosio::chain::deferred_transaction_generation_context, (sender_trx_id)(sender_id)(sender) )
FC_REFLECT(eosio::chain::transaction_shard, (shard_name)(shard_type) )
FC_REFLECT( eosio::chain::transaction_header, (expiration)(ref_block_num)(ref_block_prefix)
                                              (max_net_usage_words)(max_cpu_usage_ms)(delay_sec) )
FC_REFLECT_DERIVED( eosio::chain::transaction, (eosio::chain::transaction_header), (context_free_actions)(actions)(transaction_extensions) )
FC_REFLECT_DERIVED( eosio::chain::signed_transaction, (eosio::chain::transaction), (signatures)(context_free_data) )
FC_REFLECT_ENUM( eosio::chain::packed_transaction::compression_type, (none)(zlib))
// @ignore unpacked_trx
FC_REFLECT( eosio::chain::packed_transaction, (signatures)(compression)(packed_context_free_data)(packed_trx) )
