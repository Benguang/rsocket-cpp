// Copyright 2004-present Facebook. All Rights Reserved.

#include <gtest/gtest.h>

#include <folly/io/async/EventBase.h>

#include "rsocket/framing/FrameTransportImpl.h"
#include "rsocket/internal/SetupResumeAcceptor.h"
#include "rsocket/test/test_utils/MockDuplexConnection.h"
#include "rsocket/test/test_utils/MockFrameProcessor.h"
#include "yarpl/test_utils/Mocks.h"

using namespace rsocket;
using namespace testing;

namespace {

/*
 * Make a legitimate-looking SETUP frame.
 */
Frame_SETUP makeSetup() {
  auto version = ProtocolVersion::Current();

  Frame_SETUP frame;
  frame.header_ = FrameHeader{FrameType::SETUP, FrameFlags::EMPTY, 0};
  frame.versionMajor_ = version.major;
  frame.versionMinor_ = version.minor;
  frame.keepaliveTime_ = Frame_SETUP::kMaxKeepaliveTime;
  frame.maxLifetime_ = Frame_SETUP::kMaxLifetime;
  frame.token_ = ResumeIdentificationToken::generateNew();
  frame.metadataMimeType_ = "application/olive+oil";
  frame.dataMimeType_ = "json/vorhees";
  frame.payload_ = Payload("Test SETUP data", "Test SETUP metadata");
  return frame;
}

/*
 * Make a legitimate-looking RESUME frame.
 */
Frame_RESUME makeResume() {
  Frame_RESUME frame;
  frame.header_ = FrameHeader{FrameType::RESUME, FrameFlags::EMPTY, 0};
  frame.versionMajor_ = 1;
  frame.versionMinor_ = 0;
  frame.token_ = ResumeIdentificationToken::generateNew();
  frame.lastReceivedServerPosition_ = 500;
  frame.clientPosition_ = 300;
  return frame;
}

void setupFail(std::unique_ptr<DuplexConnection>, SetupParameters) {
  FAIL() << "setupFail() was called";
}

void resumeFail(std::unique_ptr<DuplexConnection>, ResumeParameters) {
  FAIL() << "resumeFail() was called";
}
} // namespace

TEST(SetupResumeAcceptor, ImmediateDtor) {
  folly::EventBase evb;
  SetupResumeAcceptor acceptor{&evb};
}

TEST(SetupResumeAcceptor, ImmediateClose) {
  folly::EventBase evb;
  SetupResumeAcceptor acceptor{&evb};
  acceptor.close().get();
}

TEST(SetupResumeAcceptor, CloseWithActiveConnection) {
  folly::EventBase evb;
  SetupResumeAcceptor acceptor{&evb};

  std::shared_ptr<DuplexConnection::Subscriber> outerInput;

  auto connection =
      std::make_unique<StrictMock<MockDuplexConnection>>([&](auto input) {
        outerInput = input;
        input->onSubscribe(yarpl::flowable::Subscription::empty());
      });

  ON_CALL(*connection, send_(_)).WillByDefault(Invoke([](auto&) { FAIL(); }));

  acceptor.accept(std::move(connection), setupFail, resumeFail);
  acceptor.close();

  evb.loop();

  // Normally a DuplexConnection impl would complete/error its input subscriber
  // in the destructor.  Do that manually here.
  outerInput->onComplete();
}

TEST(SetupResumeAcceptor, EarlyComplete) {
  folly::EventBase evb;
  SetupResumeAcceptor acceptor{&evb};

  auto connection =
      std::make_unique<StrictMock<MockDuplexConnection>>([](auto input) {
        input->onSubscribe(yarpl::flowable::Subscription::empty());
        input->onComplete();
      });

  acceptor.accept(std::move(connection), setupFail, resumeFail);

  evb.loop();
}

TEST(SetupResumeAcceptor, EarlyError) {
  folly::EventBase evb;
  SetupResumeAcceptor acceptor{&evb};

  auto connection =
      std::make_unique<StrictMock<MockDuplexConnection>>([](auto input) {
        input->onSubscribe(yarpl::flowable::Subscription::empty());
        input->onError(std::runtime_error("Whoops"));
      });

  acceptor.accept(std::move(connection), setupFail, resumeFail);

  evb.loop();
}

TEST(SetupResumeAcceptor, SingleSetup) {
  folly::EventBase evb;
  SetupResumeAcceptor acceptor{&evb};

  auto connection =
      std::make_unique<StrictMock<MockDuplexConnection>>([](auto input) {
        auto serializer =
            FrameSerializer::createFrameSerializer(ProtocolVersion::Current());
        input->onSubscribe(yarpl::flowable::Subscription::empty());
        input->onNext(serializer->serializeOut(makeSetup()));
        input->onComplete();
      });

  bool setupCalled = false;

  acceptor.accept(
      std::move(connection),
      [&](auto transport, auto) {
        setupCalled = true;
      },
      resumeFail);

  evb.loop();

  EXPECT_TRUE(setupCalled);
}

TEST(SetupResumeAcceptor, InvalidSetup) {
  folly::EventBase evb;
  SetupResumeAcceptor acceptor{&evb};

  auto connection =
      std::make_unique<StrictMock<MockDuplexConnection>>([](auto input) {
        auto serializer =
            FrameSerializer::createFrameSerializer(ProtocolVersion::Current());

        // Bogus keepalive time that can't be deserialized.
        auto setup = makeSetup();
        setup.keepaliveTime_ = -5;

        input->onSubscribe(yarpl::flowable::Subscription::empty());
        input->onNext(serializer->serializeOut(std::move(setup)));
        input->onComplete();
      });

  EXPECT_CALL(*connection, send_(_)).WillOnce(Invoke([](auto& buf) {
    auto serializer =
        FrameSerializer::createFrameSerializer(ProtocolVersion::Current());
    Frame_ERROR frame;
    EXPECT_TRUE(serializer->deserializeFrom(frame, buf->clone()));
    EXPECT_EQ(frame.errorCode_, ErrorCode::CONNECTION_ERROR);
  }));

  acceptor.accept(std::move(connection), setupFail, resumeFail);

  evb.loop();
}

TEST(SetupResumeAcceptor, RejectedSetup) {
  folly::EventBase evb;
  SetupResumeAcceptor acceptor{&evb};

  auto serializer =
      FrameSerializer::createFrameSerializer(ProtocolVersion::Current());

  auto connection =
      std::make_unique<StrictMock<MockDuplexConnection>>([&](auto input) {
        input->onSubscribe(yarpl::flowable::Subscription::empty());
        input->onNext(serializer->serializeOut(makeSetup()));
        input->onComplete();
      });

  EXPECT_CALL(*connection, send_(_)).WillOnce(Invoke([](auto& buf) {
    auto serializer =
        FrameSerializer::createFrameSerializer(ProtocolVersion::Current());
    Frame_ERROR frame;
    EXPECT_TRUE(serializer->deserializeFrom(frame, buf->clone()));
    EXPECT_EQ(frame.errorCode_, ErrorCode::REJECTED_SETUP);
  }));

  bool setupCalled = false;

  acceptor.accept(
      std::move(connection),
      [&](std::unique_ptr<DuplexConnection> connection, auto) {
        setupCalled = true;
        connection->send(
            serializer->serializeOut(Frame_ERROR::rejectedSetup("Oops")));
      },
      resumeFail);

  evb.loop();

  EXPECT_TRUE(setupCalled);
}

TEST(SetupResumeAcceptor, RejectedResume) {
  folly::EventBase evb;
  SetupResumeAcceptor acceptor{&evb};

  auto serializer =
      FrameSerializer::createFrameSerializer(ProtocolVersion::Current());

  auto connection =
      std::make_unique<StrictMock<MockDuplexConnection>>([&](auto input) {
        input->onSubscribe(yarpl::flowable::Subscription::empty());
        input->onNext(serializer->serializeOut(makeResume()));
        input->onComplete();
      });

  EXPECT_CALL(*connection, send_(_)).WillOnce(Invoke([](auto& buf) {
    auto serializer =
        FrameSerializer::createFrameSerializer(ProtocolVersion::Current());
    Frame_ERROR frame;
    EXPECT_TRUE(serializer->deserializeFrom(frame, buf->clone()));
    EXPECT_EQ(frame.errorCode_, ErrorCode::REJECTED_RESUME);
  }));

  bool resumeCalled = false;

  acceptor.accept(
      std::move(connection),
      setupFail,
      [&](std::unique_ptr<DuplexConnection> connection, auto) {
        resumeCalled = true;
        connection->send(serializer->serializeOut(
            Frame_ERROR::rejectedResume("Cant resume")));
      });

  evb.loop();

  EXPECT_TRUE(resumeCalled);
}
