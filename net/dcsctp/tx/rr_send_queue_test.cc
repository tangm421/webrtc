/*
 *  Copyright (c) 2021 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "net/dcsctp/tx/rr_send_queue.h"

#include <cstdint>
#include <type_traits>
#include <vector>

#include "net/dcsctp/packet/data.h"
#include "net/dcsctp/public/dcsctp_message.h"
#include "net/dcsctp/public/dcsctp_options.h"
#include "net/dcsctp/public/dcsctp_socket.h"
#include "net/dcsctp/public/types.h"
#include "net/dcsctp/testing/testing_macros.h"
#include "net/dcsctp/tx/send_queue.h"
#include "rtc_base/gunit.h"
#include "test/gmock.h"

namespace dcsctp {
namespace {
using ::testing::SizeIs;

constexpr TimeMs kNow = TimeMs(0);
constexpr StreamID kStreamID(1);
constexpr PPID kPPID(53);
constexpr size_t kMaxQueueSize = 1000;
constexpr size_t kOneFragmentPacketSize = 100;
constexpr size_t kTwoFragmentPacketSize = 101;

class RRSendQueueTest : public testing::Test {
 protected:
  RRSendQueueTest() : buf_("log: ", kMaxQueueSize) {}

  const DcSctpOptions options_;
  RRSendQueue buf_;
};

TEST_F(RRSendQueueTest, EmptyBuffer) {
  EXPECT_TRUE(buf_.IsEmpty());
  EXPECT_FALSE(buf_.Produce(kNow, kOneFragmentPacketSize).has_value());
  EXPECT_FALSE(buf_.IsFull());
}

TEST_F(RRSendQueueTest, AddAndGetSingleChunk) {
  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, {1, 2, 4, 5, 6}));

  EXPECT_FALSE(buf_.IsEmpty());
  EXPECT_FALSE(buf_.IsFull());
  absl::optional<SendQueue::DataToSend> chunk_opt =
      buf_.Produce(kNow, kOneFragmentPacketSize);
  ASSERT_TRUE(chunk_opt.has_value());
  EXPECT_TRUE(chunk_opt->data.is_beginning);
  EXPECT_TRUE(chunk_opt->data.is_end);
}

TEST_F(RRSendQueueTest, CarveOutBeginningMiddleAndEnd) {
  std::vector<uint8_t> payload(60);
  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));

  absl::optional<SendQueue::DataToSend> chunk_beg =
      buf_.Produce(kNow, /*max_size=*/20);
  ASSERT_TRUE(chunk_beg.has_value());
  EXPECT_TRUE(chunk_beg->data.is_beginning);
  EXPECT_FALSE(chunk_beg->data.is_end);

  absl::optional<SendQueue::DataToSend> chunk_mid =
      buf_.Produce(kNow, /*max_size=*/20);
  ASSERT_TRUE(chunk_mid.has_value());
  EXPECT_FALSE(chunk_mid->data.is_beginning);
  EXPECT_FALSE(chunk_mid->data.is_end);

  absl::optional<SendQueue::DataToSend> chunk_end =
      buf_.Produce(kNow, /*max_size=*/20);
  ASSERT_TRUE(chunk_end.has_value());
  EXPECT_FALSE(chunk_end->data.is_beginning);
  EXPECT_TRUE(chunk_end->data.is_end);

  EXPECT_FALSE(buf_.Produce(kNow, kOneFragmentPacketSize).has_value());
}

TEST_F(RRSendQueueTest, GetChunksFromTwoMessages) {
  std::vector<uint8_t> payload(60);
  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));
  buf_.Add(kNow, DcSctpMessage(StreamID(3), PPID(54), payload));

  absl::optional<SendQueue::DataToSend> chunk_one =
      buf_.Produce(kNow, kOneFragmentPacketSize);
  ASSERT_TRUE(chunk_one.has_value());
  EXPECT_EQ(chunk_one->data.stream_id, kStreamID);
  EXPECT_EQ(chunk_one->data.ppid, kPPID);
  EXPECT_TRUE(chunk_one->data.is_beginning);
  EXPECT_TRUE(chunk_one->data.is_end);

  absl::optional<SendQueue::DataToSend> chunk_two =
      buf_.Produce(kNow, kOneFragmentPacketSize);
  ASSERT_TRUE(chunk_two.has_value());
  EXPECT_EQ(chunk_two->data.stream_id, StreamID(3));
  EXPECT_EQ(chunk_two->data.ppid, PPID(54));
  EXPECT_TRUE(chunk_two->data.is_beginning);
  EXPECT_TRUE(chunk_two->data.is_end);
}

TEST_F(RRSendQueueTest, BufferBecomesFullAndEmptied) {
  std::vector<uint8_t> payload(600);
  EXPECT_FALSE(buf_.IsFull());
  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));
  EXPECT_FALSE(buf_.IsFull());
  buf_.Add(kNow, DcSctpMessage(StreamID(3), PPID(54), payload));
  EXPECT_TRUE(buf_.IsFull());
  // However, it's still possible to add messages. It's a soft limit, and it
  // might be necessary to forcefully add messages due to e.g. external
  // fragmentation.
  buf_.Add(kNow, DcSctpMessage(StreamID(5), PPID(55), payload));
  EXPECT_TRUE(buf_.IsFull());

  absl::optional<SendQueue::DataToSend> chunk_one = buf_.Produce(kNow, 1000);
  ASSERT_TRUE(chunk_one.has_value());
  EXPECT_EQ(chunk_one->data.stream_id, kStreamID);
  EXPECT_EQ(chunk_one->data.ppid, kPPID);

  EXPECT_TRUE(buf_.IsFull());

  absl::optional<SendQueue::DataToSend> chunk_two = buf_.Produce(kNow, 1000);
  ASSERT_TRUE(chunk_two.has_value());
  EXPECT_EQ(chunk_two->data.stream_id, StreamID(3));
  EXPECT_EQ(chunk_two->data.ppid, PPID(54));

  EXPECT_FALSE(buf_.IsFull());
  EXPECT_FALSE(buf_.IsEmpty());

  absl::optional<SendQueue::DataToSend> chunk_three = buf_.Produce(kNow, 1000);
  ASSERT_TRUE(chunk_three.has_value());
  EXPECT_EQ(chunk_three->data.stream_id, StreamID(5));
  EXPECT_EQ(chunk_three->data.ppid, PPID(55));

  EXPECT_FALSE(buf_.IsFull());
  EXPECT_TRUE(buf_.IsEmpty());
}

TEST_F(RRSendQueueTest, WillNotSendTooSmallPacket) {
  std::vector<uint8_t> payload(RRSendQueue::kMinimumFragmentedPayload + 1);
  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));

  // Wouldn't fit enough payload (wouldn't want to fragment)
  EXPECT_FALSE(
      buf_.Produce(kNow,
                   /*max_size=*/RRSendQueue::kMinimumFragmentedPayload - 1)
          .has_value());

  // Minimum fragment
  absl::optional<SendQueue::DataToSend> chunk_one =
      buf_.Produce(kNow,
                   /*max_size=*/RRSendQueue::kMinimumFragmentedPayload);
  ASSERT_TRUE(chunk_one.has_value());
  EXPECT_EQ(chunk_one->data.stream_id, kStreamID);
  EXPECT_EQ(chunk_one->data.ppid, kPPID);

  // There is only one byte remaining - it can be fetched as it doesn't require
  // additional fragmentation.
  absl::optional<SendQueue::DataToSend> chunk_two =
      buf_.Produce(kNow, /*max_size=*/1);
  ASSERT_TRUE(chunk_two.has_value());
  EXPECT_EQ(chunk_two->data.stream_id, kStreamID);
  EXPECT_EQ(chunk_two->data.ppid, kPPID);

  EXPECT_TRUE(buf_.IsEmpty());
}

TEST_F(RRSendQueueTest, DefaultsToOrderedSend) {
  std::vector<uint8_t> payload(20);

  // Default is ordered
  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));
  absl::optional<SendQueue::DataToSend> chunk_one =
      buf_.Produce(kNow, kOneFragmentPacketSize);
  ASSERT_TRUE(chunk_one.has_value());
  EXPECT_FALSE(chunk_one->data.is_unordered);

  // Explicitly unordered.
  SendOptions opts;
  opts.unordered = IsUnordered(true);
  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload), opts);
  absl::optional<SendQueue::DataToSend> chunk_two =
      buf_.Produce(kNow, kOneFragmentPacketSize);
  ASSERT_TRUE(chunk_two.has_value());
  EXPECT_TRUE(chunk_two->data.is_unordered);
}

TEST_F(RRSendQueueTest, ProduceWithLifetimeExpiry) {
  std::vector<uint8_t> payload(20);

  // Default is no expiry
  TimeMs now = kNow;
  buf_.Add(now, DcSctpMessage(kStreamID, kPPID, payload));
  now += DurationMs(1000000);
  ASSERT_TRUE(buf_.Produce(now, kOneFragmentPacketSize));

  SendOptions expires_2_seconds;
  expires_2_seconds.lifetime = DurationMs(2000);

  // Add and consume within lifetime
  buf_.Add(now, DcSctpMessage(kStreamID, kPPID, payload), expires_2_seconds);
  now += DurationMs(2000);
  ASSERT_TRUE(buf_.Produce(now, kOneFragmentPacketSize));

  // Add and consume just outside lifetime
  buf_.Add(now, DcSctpMessage(kStreamID, kPPID, payload), expires_2_seconds);
  now += DurationMs(2001);
  ASSERT_FALSE(buf_.Produce(now, kOneFragmentPacketSize));

  // A long time after expiry
  buf_.Add(now, DcSctpMessage(kStreamID, kPPID, payload), expires_2_seconds);
  now += DurationMs(1000000);
  ASSERT_FALSE(buf_.Produce(now, kOneFragmentPacketSize));

  // Expire one message, but produce the second that is not expired.
  buf_.Add(now, DcSctpMessage(kStreamID, kPPID, payload), expires_2_seconds);

  SendOptions expires_4_seconds;
  expires_4_seconds.lifetime = DurationMs(4000);

  buf_.Add(now, DcSctpMessage(kStreamID, kPPID, payload), expires_4_seconds);
  now += DurationMs(2001);

  ASSERT_TRUE(buf_.Produce(now, kOneFragmentPacketSize));
  ASSERT_FALSE(buf_.Produce(now, kOneFragmentPacketSize));
}

TEST_F(RRSendQueueTest, DiscardPartialPackets) {
  std::vector<uint8_t> payload(120);

  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));
  buf_.Add(kNow, DcSctpMessage(StreamID(2), PPID(54), payload));

  absl::optional<SendQueue::DataToSend> chunk_one =
      buf_.Produce(kNow, kOneFragmentPacketSize);
  ASSERT_TRUE(chunk_one.has_value());
  EXPECT_FALSE(chunk_one->data.is_end);
  EXPECT_EQ(chunk_one->data.stream_id, kStreamID);
  buf_.Discard(IsUnordered(false), chunk_one->data.stream_id,
               chunk_one->data.message_id);

  absl::optional<SendQueue::DataToSend> chunk_two =
      buf_.Produce(kNow, kOneFragmentPacketSize);
  ASSERT_TRUE(chunk_two.has_value());
  EXPECT_FALSE(chunk_two->data.is_end);
  EXPECT_EQ(chunk_two->data.stream_id, StreamID(2));

  absl::optional<SendQueue::DataToSend> chunk_three =
      buf_.Produce(kNow, kOneFragmentPacketSize);
  ASSERT_TRUE(chunk_three.has_value());
  EXPECT_TRUE(chunk_three->data.is_end);
  EXPECT_EQ(chunk_three->data.stream_id, StreamID(2));
  ASSERT_FALSE(buf_.Produce(kNow, kOneFragmentPacketSize));

  // Calling it again shouldn't cause issues.
  buf_.Discard(IsUnordered(false), chunk_one->data.stream_id,
               chunk_one->data.message_id);
  ASSERT_FALSE(buf_.Produce(kNow, kOneFragmentPacketSize));
}

TEST_F(RRSendQueueTest, PrepareResetStreamsDiscardsStream) {
  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, {1, 2, 3}));
  buf_.Add(kNow, DcSctpMessage(StreamID(2), PPID(54), {1, 2, 3, 4, 5}));
  EXPECT_EQ(buf_.total_bytes(), 8u);

  buf_.PrepareResetStreams(std::vector<StreamID>({StreamID(1)}));
  EXPECT_EQ(buf_.total_bytes(), 5u);
  buf_.CommitResetStreams();
  buf_.PrepareResetStreams(std::vector<StreamID>({StreamID(2)}));
  EXPECT_EQ(buf_.total_bytes(), 0u);
}

TEST_F(RRSendQueueTest, PrepareResetStreamsNotPartialPackets) {
  std::vector<uint8_t> payload(120);

  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));
  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));

  absl::optional<SendQueue::DataToSend> chunk_one = buf_.Produce(kNow, 50);
  ASSERT_TRUE(chunk_one.has_value());
  EXPECT_EQ(chunk_one->data.stream_id, kStreamID);
  EXPECT_EQ(buf_.total_bytes(), 2 * payload.size() - 50);

  StreamID stream_ids[] = {StreamID(1)};
  buf_.PrepareResetStreams(stream_ids);
  EXPECT_EQ(buf_.total_bytes(), payload.size() - 50);
}

TEST_F(RRSendQueueTest, EnqueuedItemsArePausedDuringStreamReset) {
  std::vector<uint8_t> payload(50);

  buf_.PrepareResetStreams(std::vector<StreamID>({StreamID(1)}));
  EXPECT_EQ(buf_.total_bytes(), 0u);

  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));
  EXPECT_EQ(buf_.total_bytes(), payload.size());

  EXPECT_FALSE(buf_.Produce(kNow, kOneFragmentPacketSize).has_value());
  buf_.CommitResetStreams();
  EXPECT_EQ(buf_.total_bytes(), payload.size());

  absl::optional<SendQueue::DataToSend> chunk_one = buf_.Produce(kNow, 50);
  ASSERT_TRUE(chunk_one.has_value());
  EXPECT_EQ(chunk_one->data.stream_id, kStreamID);
  EXPECT_EQ(buf_.total_bytes(), 0u);
}

TEST_F(RRSendQueueTest, CommittingResetsSSN) {
  std::vector<uint8_t> payload(50);

  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));
  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));

  absl::optional<SendQueue::DataToSend> chunk_one =
      buf_.Produce(kNow, kOneFragmentPacketSize);
  ASSERT_TRUE(chunk_one.has_value());
  EXPECT_EQ(chunk_one->data.ssn, SSN(0));

  absl::optional<SendQueue::DataToSend> chunk_two =
      buf_.Produce(kNow, kOneFragmentPacketSize);
  ASSERT_TRUE(chunk_two.has_value());
  EXPECT_EQ(chunk_two->data.ssn, SSN(1));

  StreamID stream_ids[] = {StreamID(1)};
  buf_.PrepareResetStreams(stream_ids);

  // Buffered
  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));

  EXPECT_TRUE(buf_.CanResetStreams());
  buf_.CommitResetStreams();

  absl::optional<SendQueue::DataToSend> chunk_three =
      buf_.Produce(kNow, kOneFragmentPacketSize);
  ASSERT_TRUE(chunk_three.has_value());
  EXPECT_EQ(chunk_three->data.ssn, SSN(0));
}

TEST_F(RRSendQueueTest, RollBackResumesSSN) {
  std::vector<uint8_t> payload(50);

  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));
  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));

  absl::optional<SendQueue::DataToSend> chunk_one =
      buf_.Produce(kNow, kOneFragmentPacketSize);
  ASSERT_TRUE(chunk_one.has_value());
  EXPECT_EQ(chunk_one->data.ssn, SSN(0));

  absl::optional<SendQueue::DataToSend> chunk_two =
      buf_.Produce(kNow, kOneFragmentPacketSize);
  ASSERT_TRUE(chunk_two.has_value());
  EXPECT_EQ(chunk_two->data.ssn, SSN(1));

  buf_.PrepareResetStreams(std::vector<StreamID>({StreamID(1)}));

  // Buffered
  buf_.Add(kNow, DcSctpMessage(kStreamID, kPPID, payload));

  EXPECT_TRUE(buf_.CanResetStreams());
  buf_.RollbackResetStreams();

  absl::optional<SendQueue::DataToSend> chunk_three =
      buf_.Produce(kNow, kOneFragmentPacketSize);
  ASSERT_TRUE(chunk_three.has_value());
  EXPECT_EQ(chunk_three->data.ssn, SSN(2));
}

TEST_F(RRSendQueueTest, ReturnsFragmentsForOneMessageBeforeMovingToNext) {
  std::vector<uint8_t> payload(200);
  buf_.Add(kNow, DcSctpMessage(StreamID(1), kPPID, payload));
  buf_.Add(kNow, DcSctpMessage(StreamID(2), kPPID, payload));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk1,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk1.data.stream_id, StreamID(1));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk2,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk2.data.stream_id, StreamID(1));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk3,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk3.data.stream_id, StreamID(2));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk4,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk4.data.stream_id, StreamID(2));
}

TEST_F(RRSendQueueTest, ReturnsAlsoSmallFragmentsBeforeMovingToNext) {
  std::vector<uint8_t> payload(kTwoFragmentPacketSize);
  buf_.Add(kNow, DcSctpMessage(StreamID(1), kPPID, payload));
  buf_.Add(kNow, DcSctpMessage(StreamID(2), kPPID, payload));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk1,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk1.data.stream_id, StreamID(1));
  EXPECT_THAT(chunk1.data.payload, SizeIs(kOneFragmentPacketSize));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk2,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk2.data.stream_id, StreamID(1));
  EXPECT_THAT(chunk2.data.payload,
              SizeIs(kTwoFragmentPacketSize - kOneFragmentPacketSize));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk3,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk3.data.stream_id, StreamID(2));
  EXPECT_THAT(chunk3.data.payload, SizeIs(kOneFragmentPacketSize));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk4,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk4.data.stream_id, StreamID(2));
  EXPECT_THAT(chunk4.data.payload,
              SizeIs(kTwoFragmentPacketSize - kOneFragmentPacketSize));
}

TEST_F(RRSendQueueTest, WillCycleInRoundRobinFashionBetweenStreams) {
  buf_.Add(kNow, DcSctpMessage(StreamID(1), kPPID, std::vector<uint8_t>(1)));
  buf_.Add(kNow, DcSctpMessage(StreamID(1), kPPID, std::vector<uint8_t>(2)));
  buf_.Add(kNow, DcSctpMessage(StreamID(2), kPPID, std::vector<uint8_t>(3)));
  buf_.Add(kNow, DcSctpMessage(StreamID(2), kPPID, std::vector<uint8_t>(4)));
  buf_.Add(kNow, DcSctpMessage(StreamID(3), kPPID, std::vector<uint8_t>(5)));
  buf_.Add(kNow, DcSctpMessage(StreamID(3), kPPID, std::vector<uint8_t>(6)));
  buf_.Add(kNow, DcSctpMessage(StreamID(4), kPPID, std::vector<uint8_t>(7)));
  buf_.Add(kNow, DcSctpMessage(StreamID(4), kPPID, std::vector<uint8_t>(8)));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk1,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk1.data.stream_id, StreamID(1));
  EXPECT_THAT(chunk1.data.payload, SizeIs(1));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk2,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk2.data.stream_id, StreamID(2));
  EXPECT_THAT(chunk2.data.payload, SizeIs(3));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk3,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk3.data.stream_id, StreamID(3));
  EXPECT_THAT(chunk3.data.payload, SizeIs(5));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk4,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk4.data.stream_id, StreamID(4));
  EXPECT_THAT(chunk4.data.payload, SizeIs(7));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk5,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk5.data.stream_id, StreamID(1));
  EXPECT_THAT(chunk5.data.payload, SizeIs(2));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk6,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk6.data.stream_id, StreamID(2));
  EXPECT_THAT(chunk6.data.payload, SizeIs(4));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk7,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk7.data.stream_id, StreamID(3));
  EXPECT_THAT(chunk7.data.payload, SizeIs(6));

  ASSERT_HAS_VALUE_AND_ASSIGN(SendQueue::DataToSend chunk8,
                              buf_.Produce(kNow, kOneFragmentPacketSize));
  EXPECT_EQ(chunk8.data.stream_id, StreamID(4));
  EXPECT_THAT(chunk8.data.payload, SizeIs(8));
}
}  // namespace
}  // namespace dcsctp
