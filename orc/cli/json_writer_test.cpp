/*
 * File:        json_writer_test.cpp
 * Module:      orc-cli
 * Purpose:     Unit tests for the machine-readable output emitter
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "json_writer.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

namespace {

using orc::cli::is_json_number;
using orc::cli::json_escape;
using orc::cli::JsonWriter;

// --- Escaping ---------------------------------------------------------------

TEST(JsonWriterTest, EscapesTheCharactersJsonReserves) {
  EXPECT_EQ(json_escape(R"(a "quoted" \path\)"), R"(a \"quoted\" \\path\\)");
}

TEST(JsonWriterTest, EscapesWhitespaceWithItsShortForm) {
  EXPECT_EQ(json_escape("line\nnext\tcell\r"), "line\\nnext\\tcell\\r");
}

TEST(JsonWriterTest, EscapesOtherControlCharactersAsUnicode) {
  // The unit separator is how a combo box's stored value and its label are
  // packed together, so it can reach the writer.
  EXPECT_EQ(json_escape(std::string("value\x1flabel")), "value\\u001flabel");
}

TEST(JsonWriterTest, LeavesUtf8Alone) {
  // Already valid JSON: re-encoding it would only make the output harder to
  // read, and the em dash is in the canonical strings the CLI prints.
  EXPECT_EQ(json_escape("needs a rebuild — ABI 12"),
            "needs a rebuild — ABI 12");
}

// --- Number recognition -----------------------------------------------------

TEST(JsonWriterTest, AcceptsJsonNumberSyntax) {
  EXPECT_TRUE(is_json_number("0"));
  EXPECT_TRUE(is_json_number("-1"));
  EXPECT_TRUE(is_json_number("1.500000"));
  EXPECT_TRUE(is_json_number("-0.5"));
  EXPECT_TRUE(is_json_number("1e6"));
  EXPECT_TRUE(is_json_number("1.5E-3"));
}

TEST(JsonWriterTest, RejectsWhatJsonWouldNotRead) {
  EXPECT_FALSE(is_json_number(""));
  EXPECT_FALSE(is_json_number("1.5.2"));
  EXPECT_FALSE(is_json_number(".5"));
  EXPECT_FALSE(is_json_number("1."));
  EXPECT_FALSE(is_json_number("1e"));
  EXPECT_FALSE(is_json_number("12 "));
  EXPECT_FALSE(is_json_number("nan"));
  // A build id like this is a string, not an octal number.
  EXPECT_FALSE(is_json_number("007"));
}

// --- Document structure -----------------------------------------------------

std::string written(void (*emit)(JsonWriter*)) {
  std::ostringstream out;
  JsonWriter json(out);
  emit(&json);
  json.finish();
  return out.str();
}

TEST(JsonWriterTest, SeparatesTheMembersOfAnObject) {
  EXPECT_EQ(written([](JsonWriter* json) {
              json->begin_object();
              json->member("selector", "com.example.plugin");
              json->member_bool("enabled", true);
              json->member_int("host_abi_version", 12);
              json->end_object();
            }),
            "{\n"
            "  \"selector\": \"com.example.plugin\",\n"
            "  \"enabled\": true,\n"
            "  \"host_abi_version\": 12\n"
            "}\n");
}

TEST(JsonWriterTest, WritesAnEmptyContainerOnOneLine) {
  EXPECT_EQ(written([](JsonWriter* json) {
              json->begin_object();
              json->member_strings("tags", std::vector<std::string>());
              json->key("entries");
              json->begin_array();
              json->end_array();
              json->end_object();
            }),
            "{\n"
            "  \"tags\": [],\n"
            "  \"entries\": []\n"
            "}\n");
}

TEST(JsonWriterTest, NestsObjectsUnderTheirKey) {
  EXPECT_EQ(written([](JsonWriter* json) {
              json->begin_array();
              json->begin_object();
              json->member("name", "tbc_source");
              json->key("update");
              json->begin_object();
              json->member("status", "up_to_date");
              json->end_object();
              json->end_object();
              json->end_array();
            }),
            "[\n"
            "  {\n"
            "    \"name\": \"tbc_source\",\n"
            "    \"update\": {\n"
            "      \"status\": \"up_to_date\"\n"
            "    }\n"
            "  }\n"
            "]\n");
}

TEST(JsonWriterTest, KeepsAFieldPresentWithNullWhenItHasNothingToSay) {
  // One stable object shape per command: an absent value is null, never a
  // missing key, so a script's field access cannot fail on some entries.
  EXPECT_EQ(written([](JsonWriter* json) {
              json->begin_object();
              json->member_null("update");
              json->end_object();
            }),
            "{\n  \"update\": null\n}\n");
}

TEST(JsonWriterTest, WritesNumbersVerbatim) {
  // The token is a presenter rendering of the stored value: re-rendering it
  // here could lose digits a project file kept.
  EXPECT_EQ(written([](JsonWriter* json) {
              json->begin_object();
              json->member_number("default_value", "1.500000");
              json->end_object();
            }),
            "{\n  \"default_value\": 1.500000\n}\n");
}

TEST(JsonWriterTest, QuotesATokenThatIsNotANumber) {
  // Better a string than a document no reader accepts.
  EXPECT_EQ(written([](JsonWriter* json) {
              json->begin_object();
              json->member_number("default_value", "auto");
              json->end_object();
            }),
            "{\n  \"default_value\": \"auto\"\n}\n");
}

}  // namespace
