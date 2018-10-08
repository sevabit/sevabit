// Copyright (c) 2014-2018, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include "ringct/rctSigs.h"
#include "ringct/bulletproofs.h"
#include "chaingen.h"
#include "bulletproofs.h"
#include "device/device.hpp"

using namespace epee;
using namespace crypto;
using namespace cryptonote;

//----------------------------------------------------------------------------------------------------------------------
// Tests

bool gen_bp_tx_validation_base::generate_with(std::vector<test_event_entry>& events,
      const int *out_idx,
      int mixin,
      size_t n_txes,
      const uint64_t *amounts_paid,
      bool valid,
      const bool *multi_out,
      const std::function<bool(std::vector<cryptonote::tx_source_entry> &sources,
      std::vector<cryptonote::tx_destination_entry> &destinations, size_t)> &pre_tx,
      const std::function<bool(cryptonote::transaction &tx, size_t)> &post_tx) const

{
  uint64_t ts_start = 1338224400;

  GENERATE_ACCOUNT(miner_account);
  MAKE_GENESIS_BLOCK(events, blk_0, miner_account, ts_start);

  // create 8 miner accounts, and have them mine the next 8 blocks
  int const NUM_MINERS          = 8;
  int const NUM_UNLOCKED_BLOCKS = 40;

  cryptonote::account_base miner_accounts[NUM_MINERS];
  const cryptonote::block *prev_block = &blk_0;
  cryptonote::block blocks[NUM_UNLOCKED_BLOCKS + CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW];

  for (size_t i = 0; i < NUM_MINERS; ++i)
    miner_accounts[i].generate();

  generator.set_hf_version(8);
  for (size_t n = 0; n < NUM_UNLOCKED_BLOCKS; ++n) {
    CHECK_AND_ASSERT_MES(generator.construct_block_manually(blocks[n], *prev_block, miner_accounts[n % NUM_MINERS],
        test_generator::bf_major_ver | test_generator::bf_minor_ver | test_generator::bf_timestamp | test_generator::bf_hf_version,
        8, 8, prev_block->timestamp + DIFFICULTY_BLOCKS_ESTIMATE_TIMESPAN * 2, // v2 has blocks twice as long
          crypto::hash(), 0, transaction(), std::vector<crypto::hash>(), 0),
        false, "Failed to generate block");
    events.push_back(blocks[n]);
    prev_block = blocks + n;
    LOG_PRINT_L0("Initial miner tx " << n << ": " << obj_to_json_str(blocks[n].miner_tx));
  }

  // rewind
  cryptonote::block blk_r, blk_last;
  {
    blk_last = blocks[NUM_UNLOCKED_BLOCKS - 1];
    for (size_t i = 0; i < CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW; ++i)
    {
      CHECK_AND_ASSERT_MES(generator.construct_block_manually(blocks[NUM_UNLOCKED_BLOCKS + i], blk_last, miner_account,
          test_generator::bf_major_ver | test_generator::bf_minor_ver | test_generator::bf_timestamp | test_generator::bf_hf_version,
          8, 8, blk_last.timestamp + DIFFICULTY_BLOCKS_ESTIMATE_TIMESPAN * 2, // v2 has blocks twice as long
          crypto::hash(), 0, transaction(), std::vector<crypto::hash>(), 0),
          false, "Failed to generate block");
      events.push_back(blocks[NUM_UNLOCKED_BLOCKS+i]);
      blk_last = blocks[NUM_UNLOCKED_BLOCKS+i];
    }
    blk_r = blk_last;
  }

  std::vector<transaction> rct_txes;
  cryptonote::block blk_txes;
  std::vector<crypto::hash> starting_rct_tx_hashes;

#if 0
  static const uint64_t input_amounts_available[] = {5000000000000, 30000000000000, 100000000000, 80000000000};
#endif
  for (size_t n = 0, block_index = 0; n < n_txes; ++n)
  {
#if 1
    std::vector<tx_source_entry> sources;
    std::vector<tx_destination_entry> destinations;

    cryptonote::account_base const &from = miner_accounts[n];
    cryptonote::account_base const &to   = miner_accounts[n+1];
    assert(n + 1 < NUM_MINERS);

    // NOTE: Monero tests use multiple null terminated entries in their arrays
    int amounts_paid_len = 0;
    for(int i = 0; amounts_paid[i] != (uint64_t)-1; ++i) ++amounts_paid_len;

    uint64_t change_amount;
    fill_tx_sources_and_multi_destinations(events, blk_last, from, to, amounts_paid, amounts_paid_len, TESTS_DEFAULT_FEE, 9 /*mixin*/, sources, destinations, &change_amount);

    // NOTE(loki): Monero tests presume the generated TX doesn't have change so remove it from our output.
    for (auto it = destinations.begin(); it != destinations.end(); ++it)
    {
      if (it->amount != change_amount) continue;
      destinations.erase(it);
      break;
    }

    std::unordered_map<crypto::public_key, cryptonote::subaddress_index> subaddresses;
    subaddresses[from.get_keys().m_account_address.m_spend_public_key] = {0,0};

    std::vector<crypto::secret_key> additional_tx_keys;
    cryptonote::transaction tx;
    crypto::secret_key private_tx_key;

    if (pre_tx && !pre_tx(sources, destinations, n))
    {
      MDEBUG("pre_tx returned failure");
      return false;
    }

    if (!cryptonote::construct_tx_and_get_tx_key(
        from.get_keys(),
        subaddresses,
        sources,
        destinations,
        tx_destination_entry{} /*change_addr*/,
        {} /*tx_extra*/,
        tx,
        0 /*unlock_time*/,
        private_tx_key,
        additional_tx_keys,
        true /*rct*/,
        multi_out[n] ? rct::RangeProofMultiOutputBulletproof : rct::RangeProofBulletproof))
    {
      MDEBUG("construct_tx_and_get_tx_key failure");
      return false;
    }

    rct_txes.push_back(tx);
    if (post_tx && !post_tx(rct_txes.back(), n))
    {
      MDEBUG("post_tx returned failure");
      return false;
    }

    starting_rct_tx_hashes.push_back(get_transaction_hash(rct_txes.back()));
    LOG_PRINT_L0("Test tx: " << obj_to_json_str(rct_txes.back()));

    for (int o = 0; amounts_paid[o] != (uint64_t)-1; ++o)
    {
      crypto::key_derivation derivation;
      bool r = crypto::generate_key_derivation(destinations[o].addr.m_view_public_key, private_tx_key, derivation);
      CHECK_AND_ASSERT_MES(r, false, "Failed to generate key derivation");
      crypto::secret_key amount_key;
      crypto::derivation_to_scalar(derivation, o, amount_key);
      rct::key rct_tx_mask;
      if (rct_txes.back().rct_signatures.type == rct::RCTTypeSimple || rct_txes.back().rct_signatures.type == rct::RCTTypeBulletproof)
        rct::decodeRctSimple(rct_txes.back().rct_signatures, rct::sk2rct(amount_key), o, rct_tx_mask, hw::get_device("default"));
      else
        rct::decodeRct(rct_txes.back().rct_signatures, rct::sk2rct(amount_key), o, rct_tx_mask, hw::get_device("default"));
    }

    while (amounts_paid[0] != (size_t)-1)
      ++amounts_paid;
    ++amounts_paid;

#else
    std::vector<tx_source_entry> sources;

    sources.resize(1);
    tx_source_entry& src = sources.back();

#if 1
    int const miner_reward_index = 0;
    size_t real_index_in_tx      = 0;
    for (size_t ring_index = 0; ring_index < 10; ++ring_index, ++block_index)
    {
      cryptonote::block const &block = blocks[block_index];
      src.amount                     = block.miner_tx.vout[miner_reward_index].amount;
      src.push_output(ring_index,
                      boost::get<txout_to_key>(block.miner_tx.vout[miner_reward_index].target).key,
                      src.amount);
    }

    src.real_out_tx_key         = cryptonote::get_tx_pub_key_from_extra(blocks[n].miner_tx);
    src.real_output             = 0;
    src.real_output_in_tx_index = miner_reward_index;
    src.mask                    = rct::identity();
    src.rct                     = false;

#else
    const uint64_t needed_amount = input_amounts_available[n];
    src.amount = input_amounts_available[n];
    size_t real_index_in_tx = 0;
    for (size_t m = 0; m < (size_t)10; ++m) {
      size_t index_in_tx = 0;
      for (size_t i = 0; i < blocks[m].miner_tx.vout.size(); ++i)
        if (blocks[m].miner_tx.vout[i].amount >= needed_amount)
          index_in_tx = i;
      CHECK_AND_ASSERT_MES(blocks[m].miner_tx.vout[index_in_tx].amount >= needed_amount, false, "Expected amount not found");
      src.push_output(m, boost::get<txout_to_key>(blocks[m].miner_tx.vout[index_in_tx].target).key, src.amount);
      if (m == n)
        real_index_in_tx = index_in_tx;
    }

    src.real_out_tx_key = cryptonote::get_tx_pub_key_from_extra(blocks[n].miner_tx);
    src.real_output = n;
    src.real_output_in_tx_index = real_index_in_tx;
    src.mask = rct::identity();
    src.rct = false;
#endif


    //fill outputs entry
    tx_destination_entry td;
    td.addr = miner_accounts[n].get_keys().m_account_address;
    std::vector<tx_destination_entry> destinations;
    for (int o = 0; amounts_paid[o] != (uint64_t)-1; ++o)
    {
      td.amount = amounts_paid[o];
      destinations.push_back(td);
    }

    if (pre_tx && !pre_tx(sources, destinations, n))
    {
      MDEBUG("pre_tx returned failure");
      return false;
    }

    crypto::secret_key tx_key;
    std::vector<crypto::secret_key> additional_tx_keys;
    std::unordered_map<crypto::public_key, cryptonote::subaddress_index> subaddresses;
    subaddresses[miner_accounts[n].get_keys().m_account_address.m_spend_public_key] = {0,0};
    rct_txes.resize(rct_txes.size() + 1);
    bool r = construct_tx_and_get_tx_key(miner_accounts[n].get_keys(), subaddresses, sources, destinations, cryptonote::tx_destination_entry{}, std::vector<uint8_t>(), rct_txes.back(), 0, tx_key, additional_tx_keys, true, multi_out[n] ? rct::RangeProofMultiOutputBulletproof : rct::RangeProofBulletproof);
    CHECK_AND_ASSERT_MES(r, false, "failed to construct transaction");

    if (post_tx && !post_tx(rct_txes.back(), n))
    {
      MDEBUG("post_tx returned failure");
      return false;
    }

    //events.push_back(rct_txes.back());
    starting_rct_tx_hashes.push_back(get_transaction_hash(rct_txes.back()));
    LOG_PRINT_L0("Test tx: " << obj_to_json_str(rct_txes.back()));

    for (int o = 0; amounts_paid[o] != (uint64_t)-1; ++o)
    {
      crypto::key_derivation derivation;
      bool r = crypto::generate_key_derivation(destinations[o].addr.m_view_public_key, tx_key, derivation);
      CHECK_AND_ASSERT_MES(r, false, "Failed to generate key derivation");
      crypto::secret_key amount_key;
      crypto::derivation_to_scalar(derivation, o, amount_key);
      rct::key rct_tx_mask;
      if (rct_txes.back().rct_signatures.type == rct::RCTTypeSimple || rct_txes.back().rct_signatures.type == rct::RCTTypeBulletproof)
        rct::decodeRctSimple(rct_txes.back().rct_signatures, rct::sk2rct(amount_key), o, rct_tx_mask, hw::get_device("default"));
      else
        rct::decodeRct(rct_txes.back().rct_signatures, rct::sk2rct(amount_key), o, rct_tx_mask, hw::get_device("default"));
    }

    while (amounts_paid[0] != (size_t)-1)
      ++amounts_paid;
    ++amounts_paid;
#endif
  }
  if (!valid)
    DO_CALLBACK(events, "mark_invalid_tx");
  events.push_back(rct_txes);

  generator.set_hf_version(10);
  CHECK_AND_ASSERT_MES(generator.construct_block_manually(blk_txes, blk_last, miner_account,
      test_generator::bf_major_ver | test_generator::bf_minor_ver | test_generator::bf_timestamp | test_generator::bf_tx_hashes | test_generator::bf_hf_version,
      10, 10, blk_last.timestamp + DIFFICULTY_BLOCKS_ESTIMATE_TIMESPAN * 2, // v2 has blocks twice as long
      crypto::hash(), 0, transaction(), starting_rct_tx_hashes, 0),
      false, "Failed to generate block");
  if (!valid)
    DO_CALLBACK(events, "mark_invalid_block");
  events.push_back(blk_txes);
  blk_last = blk_txes;

  return true;
}

bool gen_bp_tx_validation_base::check_bp(const cryptonote::transaction &tx, size_t tx_idx, const size_t *sizes, const char *context) const
{
  DEFINE_TESTS_ERROR_CONTEXT(context);
  CHECK_TEST_CONDITION(tx.version >= 2);
  CHECK_TEST_CONDITION(rct::is_rct_bulletproof(tx.rct_signatures.type));
  size_t n_sizes = 0, n_amounts = 0;
  for (size_t n = 0; n < tx_idx; ++n)
  {
    while (sizes[0] != (size_t)-1)
      ++sizes;
    ++sizes;
  }
  while (sizes[n_sizes] != (size_t)-1)
    n_amounts += sizes[n_sizes++];
  CHECK_TEST_CONDITION(tx.rct_signatures.p.bulletproofs.size() == n_sizes);
  CHECK_TEST_CONDITION(rct::n_bulletproof_amounts(tx.rct_signatures.p.bulletproofs) == n_amounts);
  for (size_t n = 0; n < n_sizes; ++n)
    CHECK_TEST_CONDITION(rct::n_bulletproof_amounts(tx.rct_signatures.p.bulletproofs[n]) == sizes[n]);
  return true;
}

bool gen_bp_tx_valid_1::generate(std::vector<test_event_entry>& events) const
{
  const int mixin = 6;
  const int out_idx[] = {1, -1};
  const uint64_t amounts_paid[] = {10000, (uint64_t)-1};
  const size_t bp_sizes[] = {1, (size_t)-1};
  const bool multi_out[] = {false};
  return generate_with(events, out_idx, mixin, 1, amounts_paid, true, multi_out, NULL, [&](const cryptonote::transaction &tx, size_t tx_idx){ return check_bp(tx, tx_idx, bp_sizes, "gen_bp_tx_valid_1"); });
}

bool gen_bp_tx_valid_1_1::generate(std::vector<test_event_entry>& events) const
{
  const int mixin = 6;
  const int out_idx[] = {1, -1};
  const uint64_t amounts_paid[] = {5000, 5000, (uint64_t)-1};
  const size_t bp_sizes[] = {1, 1, (size_t)-1};
  const bool multi_out[] = {false};
  return generate_with(events, out_idx, mixin, 1, amounts_paid, true, multi_out, NULL, [&](const cryptonote::transaction &tx, size_t tx_idx){ return check_bp(tx, tx_idx, bp_sizes, "gen_bp_tx_valid_1_1"); });
}

bool gen_bp_tx_valid_2::generate(std::vector<test_event_entry>& events) const
{
  const int mixin = 6;
  const int out_idx[] = {1, -1};
  const uint64_t amounts_paid[] = {5000, 5000, (uint64_t)-1};
  const size_t bp_sizes[] = {2, (size_t)-1};
  const bool multi_out[] = {true};
  return generate_with(events, out_idx, mixin, 1, amounts_paid, true, multi_out, NULL, [&](const cryptonote::transaction &tx, size_t tx_idx){ return check_bp(tx, tx_idx, bp_sizes, "gen_bp_tx_valid_2"); });
}

bool gen_bp_tx_valid_4_2_1::generate(std::vector<test_event_entry>& events) const
{
  const int mixin = 6;
  const int out_idx[] = {1, -1};
  const uint64_t amounts_paid[] = {1000, 1000, 1000, 1000, 1000, 1000, 1000, (uint64_t)-1};
  const size_t bp_sizes[] = {4, 2, 1, (size_t)-1};
  const bool multi_out[] = {true};
  return generate_with(events, out_idx, mixin, 1, amounts_paid, true, multi_out, NULL, [&](const cryptonote::transaction &tx, size_t tx_idx){ return check_bp(tx, tx_idx, bp_sizes, "gen_bp_tx_valid_4_2_1"); });
}

bool gen_bp_tx_valid_16_16::generate(std::vector<test_event_entry>& events) const
{
  const int mixin = 6;
  const int out_idx[] = {1, -1};
  const uint64_t amounts_paid[] = {1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, (uint64_t)-1};
  const size_t bp_sizes[] = {16, 16, (size_t)-1};
  const bool multi_out[] = {true};
  return generate_with(events, out_idx, mixin, 1, amounts_paid, true, multi_out, NULL, [&](const cryptonote::transaction &tx, size_t tx_idx){ return check_bp(tx, tx_idx, bp_sizes, "gen_bp_tx_valid_16_16"); });
}

bool gen_bp_txs_valid_2_and_2::generate(std::vector<test_event_entry>& events) const
{
  const int mixin = 6;
  const int out_idx[] = {1, -1};
  const uint64_t amounts_paid[] = {1000, 1000, (size_t)-1, 1000, 1000, (uint64_t)-1};
  const size_t bp_sizes[] = {2, (size_t)-1, 2, (size_t)-1};
  const bool multi_out[] = {true};
  return generate_with(events, out_idx, mixin, 2, amounts_paid, true, multi_out, NULL, [&](const cryptonote::transaction &tx, size_t tx_idx){ return check_bp(tx, tx_idx, bp_sizes, "gen_bp_txs_valid_2_2"); });
}

bool gen_bp_txs_valid_1_1_and_8_2_and_16_16_1::generate(std::vector<test_event_entry>& events) const
{
  const int mixin = 6;
  const int out_idx[] = {1, -1};
  const uint64_t amounts_paid[] = {1000, 1000, (uint64_t)-1, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, (uint64_t)-1, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, (uint64_t)-1};
  const bool multi_out[] = {false, true, true};
  const size_t bp_sizes[] = {1, 1, (size_t)-1, 8, 2, (size_t)-1, 16, 16, 1, (size_t)-1};
  return generate_with(events, out_idx, mixin, 3, amounts_paid, true, multi_out, NULL, [&](const cryptonote::transaction &tx, size_t tx_idx){ return check_bp(tx, tx_idx, bp_sizes, "gen_bp_txs_valid_1_1_and_8_2_and_16_16_1"); });
}

bool gen_bp_tx_invalid_not_enough_proofs::generate(std::vector<test_event_entry>& events) const
{
  DEFINE_TESTS_ERROR_CONTEXT("gen_bp_tx_invalid_not_enough_proofs");
  const int mixin = 6;
  const int out_idx[] = {1, -1};
  const uint64_t amounts_paid[] = {10000, (uint64_t)-1};
  const bool multi_out[] = {false};
  return generate_with(events, out_idx, mixin, 1, amounts_paid, false, multi_out, NULL, [&](cryptonote::transaction &tx, size_t idx){
    CHECK_TEST_CONDITION(tx.rct_signatures.type == rct::RCTTypeBulletproof);
    CHECK_TEST_CONDITION(!tx.rct_signatures.p.bulletproofs.empty());
    tx.rct_signatures.p.bulletproofs.pop_back();
    return true;
  });
}

bool gen_bp_tx_invalid_too_many_proofs::generate(std::vector<test_event_entry>& events) const
{
  DEFINE_TESTS_ERROR_CONTEXT("gen_bp_tx_invalid_too_many_proofs");
  const int mixin = 6;
  const int out_idx[] = {1, -1};
  const uint64_t amounts_paid[] = {10000, (uint64_t)-1};
  const bool multi_out[] = {false};
  return generate_with(events, out_idx, mixin, 1, amounts_paid, false, multi_out, NULL, [&](cryptonote::transaction &tx, size_t idx){
    CHECK_TEST_CONDITION(tx.rct_signatures.type == rct::RCTTypeBulletproof);
    CHECK_TEST_CONDITION(!tx.rct_signatures.p.bulletproofs.empty());
    tx.rct_signatures.p.bulletproofs.push_back(tx.rct_signatures.p.bulletproofs.back());
    return true;
  });
}

bool gen_bp_tx_invalid_wrong_amount::generate(std::vector<test_event_entry>& events) const
{
  DEFINE_TESTS_ERROR_CONTEXT("gen_bp_tx_invalid_wrong_amount");
  const int mixin = 6;
  const int out_idx[] = {1, -1};
  const uint64_t amounts_paid[] = {10000, (uint64_t)-1};
  const bool multi_out[] = {false};
  return generate_with(events, out_idx, mixin, 1, amounts_paid, false, multi_out, NULL, [&](cryptonote::transaction &tx, size_t idx){
    CHECK_TEST_CONDITION(tx.rct_signatures.type == rct::RCTTypeBulletproof);
    CHECK_TEST_CONDITION(!tx.rct_signatures.p.bulletproofs.empty());
    tx.rct_signatures.p.bulletproofs.back() = rct::bulletproof_PROVE(1000, rct::skGen());
    return true;
  });
}

bool gen_bp_tx_invalid_switched::generate(std::vector<test_event_entry>& events) const
{
  DEFINE_TESTS_ERROR_CONTEXT("gen_bp_tx_invalid_switched");
  const int mixin = 6;
  const int out_idx[] = {1, -1};
  const uint64_t amounts_paid[] = {5000, 5000, (uint64_t)-1};
  const bool multi_out[] = {false};
  return generate_with(events, out_idx, mixin, 1, amounts_paid, false, multi_out, NULL, [&](cryptonote::transaction &tx, size_t tx_idx){
    CHECK_TEST_CONDITION(tx.rct_signatures.type == rct::RCTTypeBulletproof);
    CHECK_TEST_CONDITION(tx.rct_signatures.p.bulletproofs.size() == 2);
    rct::Bulletproof proof = tx.rct_signatures.p.bulletproofs[0];
    tx.rct_signatures.p.bulletproofs[0] = tx.rct_signatures.p.bulletproofs[1];
    tx.rct_signatures.p.bulletproofs[1] = proof;
    return true;
  });
}

