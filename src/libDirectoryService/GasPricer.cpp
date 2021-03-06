/*
 * Copyright (c) 2018 Zilliqa
 * This source code is being disclosed to you solely for the purpose of your
 * participation in testing Zilliqa. You may view, compile and run the code for
 * that purpose and pursuant to the protocols and algorithms that are programmed
 * into, and intended by, the code. You may not do anything else with the code
 * without express permission from Zilliqa Research Pte. Ltd., including
 * modifying or publishing the code (or any part of it), and developing or
 * forming another public or private blockchain network. This source code is
 * provided 'as is' and no warranties are given as to title or non-infringement,
 * merchantability or fitness for purpose and, to the extent permitted by law,
 * all liability for your use of the code is disclaimed. Some programs in this
 * code are governed by the GNU General Public License v3.0 (available at
 * https://www.gnu.org/licenses/gpl-3.0.en.html) ('GPLv3'). The programs that
 * are governed by GPLv3.0 are those programs that are located in the folders
 * src/depends and tests/depends and which include a reference to GPLv3 in their
 * program files.
 */

#include "DirectoryService.h"
#include "libMediator/Mediator.h"
#include "libUtils/Logger.h"

using namespace std;
using namespace boost::multiprecision;

uint128_t DirectoryService::GetNewGasPrice() {
  LOG_MARKER();

  uint64_t loBlockNum =
      m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetEpochNum();
  uint64_t hiBlockNum =
      m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetBlockNum();
  uint64_t totalBlockNum = 0;
  uint64_t fullBlockNum = 0;

  for (uint64_t i = loBlockNum; i <= hiBlockNum; ++i) {
    uint128_t gasUsed =
        m_mediator.m_txBlockChain.GetBlock(i).GetHeader().GetGasUsed();
    uint128_t gasLimit =
        m_mediator.m_txBlockChain.GetBlock(i).GetHeader().GetGasLimit();
    if (gasUsed >= gasLimit * GAS_CONGESTION_PERCENT / 100) {
      fullBlockNum++;
    }
    totalBlockNum++;
  }

  if (fullBlockNum < totalBlockNum * UNFILLED_PERCENT_LOW / 100) {
    return GetDecreasedGasPrice();
  } else if (fullBlockNum > totalBlockNum * UNFILLED_PERCENT_HIGH / 100) {
    return GetIncreasedGasPrice();
  }
  return max(m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetGasPrice(),
             PRECISION_MIN_VALUE);
}

uint128_t DirectoryService::GetHistoricalMeanGasPrice() {
  uint64_t curDSBlockNum =
      m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetBlockNum();
  uint64_t lowDSBlockNum = (curDSBlockNum > MEAN_GAS_PRICE_DS_NUM)
                               ? (curDSBlockNum - MEAN_GAS_PRICE_DS_NUM)
                               : 0;
  uint64_t totalBlockNum = 0;
  uint128_t totalGasPrice = 0;
  for (uint64_t i = curDSBlockNum; i >= lowDSBlockNum; --i) {
    if (i == 0) {
      break;
    }
    if (!SafeMath<uint128_t>::add(
            totalGasPrice,
            m_mediator.m_dsBlockChain.GetBlock(i).GetHeader().GetGasPrice(),
            totalGasPrice)) {
      continue;
    }
    totalBlockNum++;
  }
  uint128_t ret;
  if (!SafeMath<uint128_t>::div(totalGasPrice, totalBlockNum, ret)) {
    return m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetGasPrice();
  }
  return ret;
}

uint128_t DirectoryService::GetIncreasedGasPrice() {
  LOG_MARKER();

  uint128_t mean_val = GetHistoricalMeanGasPrice();
  uint128_t lowerbound, upperbound;
  bool smflag = true;  // SafeMath Flag
  // upperbound = (PRECISION_MIN_VALUE + GAS_PRICE_RAISE_RATIO_UPPER) /
  // PRECISION_MIN_VALUE * mean_val lowerbound = (PRECISION_MIN_VALUE +
  // ({GAS_PRICE_RAISE_RATIO_LOWER} or {UPPER/2} if {LOWER > UPPER})) /
  // PRECISION_MIN_VALUE * mean_val
  if (!SafeMath<uint128_t>::mul(
          mean_val, PRECISION_MIN_VALUE + GAS_PRICE_RAISE_RATIO_UPPER,
          upperbound)) {
    smflag = false;
  }
  if (smflag &&
      !SafeMath<uint128_t>::div(upperbound, PRECISION_MIN_VALUE, upperbound)) {
    smflag = false;
  }
  if (!smflag) {
    upperbound = mean_val;
    lowerbound = upperbound;
  } else {
    lowerbound = (PRECISION_MIN_VALUE +
                  ((GAS_PRICE_RAISE_RATIO_LOWER <= GAS_PRICE_RAISE_RATIO_UPPER)
                       ? GAS_PRICE_RAISE_RATIO_LOWER
                       : (GAS_PRICE_RAISE_RATIO_UPPER / 2))) /
                 PRECISION_MIN_VALUE * mean_val;
  }

  multiset<uint128_t> gasProposals;
  for (const auto& soln : m_allDSPoWs) {
    if (soln.second.gasPrice <= upperbound) {
      gasProposals.emplace(soln.second.gasPrice);
    }
  }
  if (gasProposals.empty()) {
    return GetHistoricalMeanGasPrice();
  }

  // Get median value
  const size_t n = gasProposals.size();
  auto iter = gasProposals.cbegin();
  std::advance(iter, n / 2);

  uint128_t median_val;

  if (n % 2 == 0) {
    const auto iter2 = iter--;
    median_val = (*iter + *iter2) / 2;
  } else {
    median_val = *iter;
  }

  return max(max(lowerbound, min(median_val, upperbound)), PRECISION_MIN_VALUE);
}

uint128_t DirectoryService::GetDecreasedGasPrice() {
  LOG_MARKER();

  uint128_t mean_val = GetHistoricalMeanGasPrice();
  uint128_t decreased_val;

  bool smflag = true;  // SafeMath Flag
  // increased value = (PRECISION_MIN_VALUE + GAS_PRICE_RAISE_RATIO_UPPER) /
  // PRECISION_MIN_VALUE * mean_val
  if (!SafeMath<uint128_t>::mul(mean_val,
                                PRECISION_MIN_VALUE - GAS_PRICE_DROP_RATIO,
                                decreased_val)) {
    smflag = false;
  }
  if (smflag && !SafeMath<uint128_t>::div(decreased_val, PRECISION_MIN_VALUE,
                                          decreased_val)) {
    smflag = false;
  }
  if (!smflag) {
    decreased_val = mean_val;
  }

  return max(PRECISION_MIN_VALUE, decreased_val);
}

bool DirectoryService::VerifyGasPrice(const uint128_t& gasPrice) {
  LOG_MARKER();

  uint128_t myGasPrice = GetNewGasPrice();

  uint128_t allowedUpper, allowedLower;

  bool smflag = true;  // SafeMath Flag
  // allowedUpper = (PRECISION_MIN_VALUE + GAS_PRICE_TOLERANCE) /
  // PRECISION_MIN_VALUE * myGasPrice
  if (!SafeMath<uint128_t>::mul(myGasPrice,
                                PRECISION_MIN_VALUE + GAS_PRICE_TOLERANCE,
                                allowedUpper)) {
    smflag = false;
  }
  if (smflag && !SafeMath<uint128_t>::div(allowedUpper, PRECISION_MIN_VALUE,
                                          allowedUpper)) {
    smflag = false;
  }
  if (!smflag) {
    allowedUpper = gasPrice;
  }

  smflag = true;
  // allowedLower = (PRECISION_MIN_VALUE - GAS_PRICE_TOLERANCE) /
  // PRECISION_MIN_VALUE * myGasPrice
  if (!SafeMath<uint128_t>::mul(myGasPrice,
                                PRECISION_MIN_VALUE - GAS_PRICE_TOLERANCE,
                                allowedLower)) {
    smflag = false;
  }
  if (smflag && !SafeMath<uint128_t>::div(allowedLower, PRECISION_MIN_VALUE,
                                          allowedLower)) {
    smflag = false;
  }
  if (!smflag) {
    allowedLower = gasPrice;
  }

  if ((gasPrice <= allowedUpper) && (gasPrice >= allowedLower)) {
    return true;
  }

  LOG_GENERAL(WARNING, "Received: " << gasPrice
                                    << " my calculated: " << myGasPrice
                                    << ", allowedUpper: " << allowedUpper
                                    << ", allowedLower: " << allowedLower);
  return false;
}