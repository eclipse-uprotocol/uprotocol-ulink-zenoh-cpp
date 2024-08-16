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
#include <up-cpp/datamodel/builder/Uuid.h>
#include <up-cpp/datamodel/serializer/UUri.h>
#include <up-cpp/datamodel/validator/UUri.h>

#include <iostream>

#include "up-transport-zenoh-cpp/ZenohUTransport.h"

namespace {

using namespace uprotocol;

constexpr std::string_view ZENOH_CONFIG_FILE = BUILD_REALPATH_ZENOH_CONF;

constexpr std::string_view ENTITY_URI_STR = "//test0/10001/1/0";
constexpr std::string_view TOPIC_URI_STR = "//test0/10001/1/8000";

class TestZenohUTransport : public testing::Test {
protected:
	// Run once per TEST_F.
	// Used to set up clean environments per test.
	void SetUp() override {}
	void TearDown() override {}

	// Run once per execution of the test application.
	// Used for setup of all tests. Has access to this instance.
	TestZenohUTransport() = default;
	~TestZenohUTransport() = default;

	// Run once per execution of the test application.
	// Used only for global setup outside of tests.
	static void SetUpTestSuite() {}
	static void TearDownTestSuite() {}
};

uprotocol::v1::UUri create_uuri(const std::string& authority, uint32_t ue_id,
                                uint32_t ue_version_major,
                                uint32_t resource_id) {
	uprotocol::v1::UUri uuri;
	uuri.set_authority_name(authority);
	uuri.set_ue_id(ue_id);
	uuri.set_ue_version_major(ue_version_major);
	uuri.set_resource_id(resource_id);

	return uuri;
}

v1::UUri create_uuri(std::string_view serialized) {
	return datamodel::serializer::uri::AsString::deserialize(
	    static_cast<std::string>(serialized));
}

// TODO(sashacmc): config generation
TEST_F(TestZenohUTransport, ConstructDestroy) {
	std::cout << ZENOH_CONFIG_FILE << std::endl;

	zenoh::init_logger();

	auto transport = std::make_shared<transport::ZenohUTransport>(
	    create_uuri(ENTITY_URI_STR), ZENOH_CONFIG_FILE);
}

struct ExposeKeyString : public transport::ZenohUTransport {
	template <typename... Args>
	static auto toZenohKeyString(Args&&... args) {
		return transport::ZenohUTransport::toZenohKeyString(
		    std::forward<Args>(args)...);
	}
};

TEST_F(TestZenohUTransport, toZenohKeyString) {
	EXPECT_TRUE(
	    (std::is_base_of_v<transport::ZenohUTransport, ExposeKeyString>));

	EXPECT_EQ(ExposeKeyString::toZenohKeyString(
	        "10.23.45.67", create_uuri("", 0x10AB, 3, 0x80CD), std::nullopt),
	    "up/10.23.45.67/10AB/3/80CD/{}/{}/{}/{}");

	EXPECT_EQ(ExposeKeyString::toZenohKeyString(
	        "", create_uuri("192.168.1.100", 0x10AB, 3, 0x80CD), std::nullopt),
	    "up/192.168.1.100/10AB/3/80CD/{}/{}/{}/{}");

	EXPECT_EQ(ExposeKeyString::toZenohKeyString(
	              "", create_uuri("192.168.1.100", 0x10AB, 3, 0x80CD),
	              create_uuri("192.168.1.101", 0x20EF, 4, 0)),
	          "up/192.168.1.100/10AB/3/80CD/192.168.1.101/20EF/4/0");

	EXPECT_EQ(ExposeKeyString::toZenohKeyString(
	              "", create_uuri("*", 0xFFFF, 0xFF, 0xFFFF),
	              create_uuri("192.168.1.101", 0x20EF, 4, 0)),
	          "up/*/*/*/*/192.168.1.101/20EF/4/0");

	EXPECT_EQ(ExposeKeyString::toZenohKeyString(
	              "", create_uuri("my-host1", 0x10AB, 3, 0),
	              create_uuri("my-host2", 0x20EF, 4, 0xB)),
	          "up/my-host1/10AB/3/0/my-host2/20EF/4/B");

	EXPECT_EQ(ExposeKeyString::toZenohKeyString(
	              "", create_uuri("*", 0xFFFF, 0xFF, 0xFFFF),
	              create_uuri("my-host2", 0x20EF, 4, 0xB)),
	          "up/*/*/*/*/my-host2/20EF/4/B");

	EXPECT_EQ(ExposeKeyString::toZenohKeyString(
	              "", create_uuri("*", 0xFFFF, 0xFF, 0xFFFF),
	              create_uuri("[::1]", 0xFFFF, 0xFF, 0xFFFF)),
	          "up/*/*/*/*/[::1]/*/*/*");
}

}  // namespace
