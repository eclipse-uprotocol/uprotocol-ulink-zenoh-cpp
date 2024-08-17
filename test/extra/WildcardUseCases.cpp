// SPDX-FileCopyrightText: 2024 Contributors to the Eclipse Foundation
//
// See the NOTICE file(s) distributed with this work for additional
// information regarding copyright ownership.
//
// This program and the accompanying materials are made available under the
// terms of the Apache License Version 2.0 which is available at
// https://www.apache.org/licenses/LICENSE-2.0
//
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <up-cpp/communication/NotificationSink.h>
#include <up-cpp/communication/NotificationSource.h>
#include <up-cpp/communication/Publisher.h>
#include <up-cpp/communication/RpcClient.h>
#include <up-cpp/communication/RpcServer.h>
#include <up-cpp/communication/Subscriber.h>
#include <up-cpp/datamodel/builder/Payload.h>
#include <up-cpp/datamodel/serializer/UUri.h>
#include <up-transport-zenoh-cpp/ZenohUTransport.h>

#include <set>
#include <tuple>

using namespace std::chrono_literals;

namespace {

using namespace uprotocol;
using Transport = transport::UTransport;
using ZTransport = transport::ZenohUTransport;

constexpr std::string_view ZENOH_CONFIG_FILE = BUILD_REALPATH_ZENOH_CONF;

constexpr std::string_view TRANSPORT_ENTITY = "//wildcard-test/10001/1/0";

constexpr std::string_view ALL_WILDCARD_FILTER = "//*/FFFF/FF/FFFF";

constexpr std::string_view PUBLISH_TOPIC = "//wildcard-test-pub/C0FFEE/2/A7EA";
constexpr std::string_view NOTIFICATION_TOPIC = "//wildcard-test-notify/10000/3/8002";
constexpr std::string_view RPC_METHOD = "//wildcard-test-rpc/FFFEFFFE/FE/7FFF";

v1::UUri makeUuri(const std::string_view uri) {
	return datamodel::serializer::uri::AsString::deserialize(
			static_cast<std::string>(uri));
}

std::shared_ptr<Transport> makeTransport(const v1::UUri& uuri) {
	SCOPED_TRACE(datamodel::serializer::uri::AsString::serialize(uuri));
	std::shared_ptr<Transport> transport;
	EXPECT_NO_THROW(transport = std::make_shared<ZTransport>(uuri, ZENOH_CONFIG_FILE););
	return transport;
}

std::shared_ptr<Transport> makeTransport(const std::string_view uri) {
	return makeTransport(makeUuri(uri));
}

datamodel::builder::Payload empty() {
	return {std::string(), v1::UPayloadFormat::UPAYLOAD_FORMAT_UNSPECIFIED};
}

class WildcardUseCases : public testing::Test {
protected:

	// Run once per TEST_F.
	// Used to set up clean environments per test.
	void SetUp() override { }
	void TearDown() override { }

	// Run once per execution of the test application.
	// Used for setup of all tests. Has access to this instance.
	WildcardUseCases() = default;
	~WildcardUseCases() = default;

	// Run once per execution of the test application.
	// Used only for global setup outside of tests.
	static void SetUpTestSuite() {}
	static void TearDownTestSuite() {}
};

TEST_F(WildcardUseCases, AuditingObserver) {
	auto transport = makeTransport(TRANSPORT_ENTITY);
	EXPECT_TRUE(transport);

	std::mutex observer_mtx;
	std::vector<v1::UMessage> observed_messages;
	auto observer = transport->registerListener(
			[&observed_messages, &observer_mtx](auto m) {
				std::lock_guard lock(observer_mtx);
				observed_messages.push_back(m);
			},
			makeUuri(ALL_WILDCARD_FILTER), makeUuri(ALL_WILDCARD_FILTER));
	EXPECT_TRUE(observer);

	// RPC
	std::atomic<int> rpc_count = 0;
	auto maybe_rpc_server = communication::RpcServer::create(
			transport, makeUuri(RPC_METHOD),
			[&rpc_count](auto m) { ++rpc_count; return std::nullopt; });
	EXPECT_TRUE(maybe_rpc_server);
	if (maybe_rpc_server) {
		EXPECT_TRUE(maybe_rpc_server.value());
	} else {
		EXPECT_EQ(maybe_rpc_server.error().code(), v1::UCode::OK);
		if (maybe_rpc_server.error().has_message()) {
			EXPECT_EQ(maybe_rpc_server.error().message(), "");
		}
	}

	communication::RpcClient rpc_client(transport, makeUuri(RPC_METHOD),
	                                    v1::UPriority::UPRIORITY_CS4, 250ms);

	// PUBSUB
	communication::Publisher publisher(
			transport, makeUuri(PUBLISH_TOPIC),
			v1::UPayloadFormat::UPAYLOAD_FORMAT_UNSPECIFIED);

	std::atomic<int> pubsub_count = 0;
	auto maybe_subscriber = communication::Subscriber::subscribe(
			transport, makeUuri(PUBLISH_TOPIC),
			[&pubsub_count](auto m) { ++pubsub_count; });
	EXPECT_TRUE(maybe_subscriber);
	EXPECT_TRUE(maybe_subscriber.value());

	// NOTIFY
	communication::NotificationSource notify_source(transport,
			makeUuri(NOTIFICATION_TOPIC), makeUuri(TRANSPORT_ENTITY));

	std::atomic<int> notify_count = 0;
	auto maybe_notify_sink = communication::NotificationSink::create(
			transport, [&notify_count](auto m) { ++notify_count; },
			makeUuri(NOTIFICATION_TOPIC));
	EXPECT_TRUE(maybe_notify_sink);
	EXPECT_TRUE(maybe_notify_sink.value());

	// Run everything
	EXPECT_EQ(observed_messages.size(), 0);
	EXPECT_EQ(notify_count, 0);
	EXPECT_EQ(pubsub_count, 0);
	EXPECT_EQ(rpc_count, 0);

	std::ignore = publisher.publish(empty());
	std::ignore = publisher.publish(empty());
	std::ignore = notify_source.notify();
	std::ignore = notify_source.notify();
	std::ignore = notify_source.notify();
	auto request_future = rpc_client.invokeMethod();
	request_future.wait_for(1s);

	EXPECT_EQ(observed_messages.size(), 7);
	EXPECT_EQ(notify_count, 3);
	EXPECT_EQ(pubsub_count, 2);
	EXPECT_EQ(rpc_count, 1);

	std::set<v1::UMessageType> observed_types;
	for (auto& message : observed_messages) {
		observed_types.insert(message.attributes().type());
	}

	EXPECT_EQ(observed_types.size(), 4);
	EXPECT_EQ(observed_types.count(v1::UMessageType::UMESSAGE_TYPE_PUBLISH), 1);
	EXPECT_EQ(observed_types.count(v1::UMessageType::UMESSAGE_TYPE_REQUEST), 1);
	EXPECT_EQ(observed_types.count(v1::UMessageType::UMESSAGE_TYPE_RESPONSE), 1);
	EXPECT_EQ(observed_types.count(v1::UMessageType::UMESSAGE_TYPE_NOTIFICATION), 1);
}

}  // namespace
