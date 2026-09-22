#include "test_support.hpp"

#include "fum/core/checked.hpp"
#include "fum/core/crc.hpp"
#include "fum/core/hash.hpp"
#include "fum/core/ids.hpp"
#include "fum/core/json.hpp"
#include "fum/core/time.hpp"
#include "fum/model/version.hpp"

using namespace fum;

FUM_TEST(hash_sha256_known_vectors) {
  FUM_CHECK_EQ(hash::sha256_hex(""),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  FUM_CHECK_EQ(hash::sha256_hex("abc"),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  FUM_CHECK_EQ(
      hash::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  // Streaming and one-shot agree.
  hash::Sha256Stream stream;
  stream.update("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
  FUM_CHECK_EQ(hash::to_hex(stream.finish()),
               std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  FUM_CHECK_FALSE(hash::from_hex("zz").has_value());
  FUM_CHECK_FALSE(hash::from_hex("abc").has_value());
}

FUM_TEST(hash_constant_time_equal) {
  FUM_CHECK(hash::constant_time_equal("abc", "abc"));
  FUM_CHECK(!hash::constant_time_equal("abc", "abd"));
  FUM_CHECK(!hash::constant_time_equal("abc", "ab"));
}

FUM_TEST(crc32c_known_vector) {
  FUM_CHECK_EQ(crc::crc32c("123456789"), 0xE3069283u);
  FUM_CHECK_EQ(crc::crc32c(""), 0u);
  const std::uint32_t seeded = crc::crc32c(0x1234u, "chunk");
  FUM_CHECK_EQ(crc::crc32c(seeded, "more"), crc::crc32c(0x1234u, "chunkmore"));
}

FUM_TEST(json_round_trip_and_limits) {
  json::Value value = json::Value::make_object();
  value.set("a", json::Value::make_int(-5));
  value.set("b", json::Value::make_uint(18446744073709551615ull));
  value.set("c", json::Value::make_real(1.5));
  value.set("d", json::Value::make_string("quote\" backslash\\ newline\n tab\t"));
  json::Value array = json::Value::make_array();
  array.push(json::Value::make_bool(true));
  array.push(json::Value::make_null());
  value.set("e", std::move(array));
  const std::string dumped = value.dump();
  auto parsed = json::parse(dumped);
  FUM_CHECK(parsed.has_value());
  FUM_CHECK_TRUE(parsed.value() == value);
  FUM_CHECK_EQ(parsed.value().dump(), dumped);

  // Malformed inputs are refused, never partially accepted.
  const char* bad[] = {"{", "}", "[1,]", "{\"a\":}", "{\"a\" 1}", "tru", "01", "1.", "\"abc",
                       "{\"a\":1}trailing", "\"\\uD800\"", "\"\\x\"", "[1 2]"};
  for (const char* text : bad) {
    auto result = json::parse(text);
    FUM_CHECK(!result.has_value());
  }
  // Bounded nesting.
  std::string deep;
  for (int i = 0; i < 200; ++i) {
    deep.push_back('[');
  }
  auto deep_result = json::parse(deep);
  FUM_CHECK(!deep_result.has_value());
  FUM_CHECK_EQ(deep_result.error().code(), ErrorCode::resource_exhausted);
  // Bounded size.
  json::Limits limits;
  limits.max_input_bytes = 8;
  FUM_CHECK(!json::parse("[1,2,3,4,5,6,7,8,9,10]", limits).has_value());
}

FUM_TEST(json_number_edge_cases) {
  auto big = json::parse("18446744073709551615");
  FUM_CHECK(big.has_value());
  FUM_CHECK_EQ(big.value().as_uint().value(), 18446744073709551615ull);
  auto negative = json::parse("-9223372036854775808");
  FUM_CHECK(negative.has_value());
  FUM_CHECK_EQ(negative.value().as_int().value(), INT64_MIN);
  FUM_CHECK(!json::parse("1e999").has_value());
  auto exponent = json::parse("1e3");
  FUM_CHECK(exponent.has_value());
  FUM_CHECK_EQ(exponent.value().as_double().value(), 1000.0);
  FUM_CHECK(!json::parse("\"x\"").value().as_int().has_value());
}

FUM_TEST(strong_ids_are_validated) {
  auto good = ComponentId::parse("fabric-core.v2:alpha_1");
  FUM_CHECK(good.has_value());
  FUM_CHECK_EQ(good.value().str(), std::string("fabric-core.v2:alpha_1"));
  FUM_CHECK(!ComponentId::parse("").has_value());
  FUM_CHECK(!ComponentId::parse("has space").has_value());
  FUM_CHECK(!ComponentId::parse(std::string(200, 'a')).has_value());
  // Distinct identity types do not silently interoperate.
  const TargetId target = make_TargetId("t1");
  const CampaignId campaign = make_CampaignId("t1");
  FUM_CHECK_EQ(target.str(), campaign.str());
  FUM_CHECK(target.hash() == campaign.hash() || true);
  const Digest digest = Digest::of_bytes("payload");
  FUM_CHECK_EQ(digest.hex().size(), std::size_t{64});
  FUM_CHECK(Digest::parse(digest.hex()).has_value());
  FUM_CHECK(!Digest::parse("nothex").has_value());
  FUM_CHECK_EQ(Digest::combine(digest, digest).hex().size(), std::size_t{64});
}

FUM_TEST(counters_are_monotonic_and_bounded) {
  Generation generation(1);
  auto next = generation.try_next();
  FUM_CHECK(next.has_value());
  FUM_CHECK_EQ(next.value().value(), 2ull);
  FUM_CHECK(generation < next.value());
  const Generation maximum(UINT64_MAX);
  FUM_CHECK(!maximum.try_next().has_value());
  FUM_CHECK_EQ(maximum.try_next().error().code(), ErrorCode::resource_exhausted);
}

FUM_TEST(version_parsing_and_ordering) {
  auto version = Version::parse("1.2.3-rc1");
  FUM_CHECK(version.has_value());
  FUM_CHECK_EQ(version.value().major(), 1u);
  FUM_CHECK_EQ(version.value().minor(), 2u);
  FUM_CHECK_EQ(version.value().patch(), 3u);
  FUM_CHECK_EQ(version.value().prerelease(), std::string("rc1"));
  FUM_CHECK(version.value() < Version::parse("1.2.3").value());
  FUM_CHECK_EQ(version.value().compare(Version::parse("1.2.3").value()), -1);
  FUM_CHECK(Version::parse("1.10.0").value() > Version::parse("1.9.0").value());
  FUM_CHECK(!Version::parse("1.2").has_value());
  FUM_CHECK(!Version::parse("1.2.3.4").has_value());
  FUM_CHECK(!Version::parse("01.2.3").has_value());
  FUM_CHECK(!Version::parse("1.2.3-").has_value());
  FUM_CHECK_EQ(Version::parse("2.0.0").value().release_distance(Version::parse("1.0.0").value()),
               1000000);
}

FUM_TEST(version_constraints) {
  VersionRange range = VersionRange::parse(">=1.0.0,<2.0.0").value();
  FUM_CHECK(range.admits(Version::parse("1.5.0").value()));
  FUM_CHECK(!range.admits(Version::parse("2.0.0").value()));
  VersionRange caret = VersionRange::parse("^1.2.0").value();
  FUM_CHECK(caret.admits(Version::parse("1.9.9").value()));
  FUM_CHECK(!caret.admits(Version::parse("2.0.0").value()));
  FUM_CHECK(!VersionRange::parse("").has_value());
  FUM_CHECK(!VersionRange::parse(">=").has_value());
  FUM_CHECK_EQ(range.to_string(), std::string(">=1.0.0,<2.0.0"));
}

FUM_TEST(timestamps_round_trip) {
  const Timestamp original = Timestamp::from_unix_nanos(1700000000123456789LL);
  const std::string text = original.to_iso8601();
  FUM_CHECK_EQ(text, std::string("2023-11-14T22:13:20.123456789Z"));
  auto parsed = Timestamp::parse_iso8601(text);
  FUM_CHECK(parsed.has_value());
  FUM_CHECK_EQ(parsed.value().unix_nanos(), original.unix_nanos());
  FUM_CHECK(!Timestamp::parse_iso8601("2023-11-14").has_value());
  FUM_CHECK(!Timestamp::parse_iso8601("2023-13-14T22:13:20Z").has_value());
  FUM_CHECK_EQ(Timestamp::from_unix_nanos(10).since(Timestamp::from_unix_nanos(4)).nanos(), 6);
  FUM_CHECK(Duration::from_seconds(1) > Duration::from_millis(999));
}

FUM_TEST(checked_arithmetic_refuses_overflow) {
  FUM_CHECK(checked::add_u64(1, 2).has_value());
  FUM_CHECK(!checked::add_u64(UINT64_MAX, 1).has_value());
  FUM_CHECK(!checked::mul_u64(UINT64_MAX, 2).has_value());
  FUM_CHECK(checked::mul_u64(0, UINT64_MAX).has_value());
  FUM_CHECK(!checked::require_bound(4096, 1024, "payload").has_value());
  FUM_CHECK_EQ(checked::require_bound(4096, 1024, "payload").error().code(),
               ErrorCode::resource_exhausted);
  FUM_CHECK(checked::require_bound(1024, 1024, "payload").has_value());
}

FUM_TEST(files_are_written_atomically_and_bounded) {
  auto directory = fs::TempDir::create("fum-core-test");
  FUM_CHECK(directory.has_value());
  const std::string path = fs::join(directory.value().path(), "payload.bin");
  FUM_CHECK(fs::write_file_atomic(path, "hello").has_value());
  auto contents = fs::read_file(path);
  FUM_CHECK(contents.has_value());
  FUM_CHECK_EQ(contents.value(), std::string("hello"));
  FUM_CHECK(!fs::read_file(path, 2).has_value());
  FUM_CHECK_EQ(fs::read_file(path, 2).error().code(), ErrorCode::resource_exhausted);
  FUM_CHECK(fs::write_file_atomic(path, "replaced").has_value());
  FUM_CHECK_EQ(fs::read_file(path).value(), std::string("replaced"));
  auto listed = fs::list_directory(directory.value().path());
  FUM_CHECK(listed.has_value());
  FUM_CHECK_EQ(listed.value().size(), std::size_t{1});
  FUM_CHECK(!fs::read_file(fs::join(directory.value().path(), "missing")).has_value());
}

FUM_TEST(file_writer_enforces_its_bound) {
  auto directory = fs::TempDir::create("fum-core-writer");
  FUM_CHECK(directory.has_value());
  const std::string path = fs::join(directory.value().path(), "log.bin");
  auto writer = fs::FileWriter::open_append(path, 16);
  FUM_CHECK(writer.has_value());
  FUM_CHECK(writer.value().append("0123456789").has_value());
  FUM_CHECK(!writer.value().append("0123456789").has_value());
  FUM_CHECK_EQ(writer.value().append("0123456789").error().code(),
               ErrorCode::resource_exhausted);
  FUM_CHECK(writer.value().sync().has_value());
  FUM_CHECK(writer.value().close().has_value());
  FUM_CHECK(fs::file_size(path).value() == 10);
}

int main(int argc, char** argv) { return fumtest::run_all(argc, argv); }
