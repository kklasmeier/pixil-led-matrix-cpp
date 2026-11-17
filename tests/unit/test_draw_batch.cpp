#include <cstdio>
#include <fstream>
#include <sstream>

#include "check.h"
#include "emit/colors.h"
#include "emit/draw_batch.h"
#include "util/json.h"

using namespace pixil;

namespace {

Value from_json(const json::Node& n) {
    switch (n.type) {
        case json::Node::Type::Null: return Value::none();
        case json::Node::Type::Bool: return Value::boolean(n.b);
        case json::Node::Type::Int: return Value::integer(n.i);
        case json::Node::Type::Float: return Value::real(n.d);
        case json::Node::Type::String: return Value::str(n.s);
        default: throw std::runtime_error("unsupported golden arg");
    }
}

std::string to_hex(const std::vector<uint8_t>& bytes, size_t from) {
    std::string out;
    char buf[3];
    for (size_t i = from; i < bytes.size(); ++i) {
        std::snprintf(buf, sizeof buf, "%02x", bytes[i]);
        out += buf;
    }
    return out;
}

const char* golden_path() {
    const char* env = std::getenv("PIXIL_GOLDEN_DIR");
    static std::string path = std::string(env ? env : "tests/golden") + "/draw_batch.jsonl";
    return path.c_str();
}

}  // namespace

TEST(base64_vectors) {
    auto enc = [](const std::string& s) {
        return base64_encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    };
    CHECK_EQ(enc(""), "");
    CHECK_EQ(enc("f"), "Zg==");
    CHECK_EQ(enc("fo"), "Zm8=");
    CHECK_EQ(enc("foo"), "Zm9v");
    CHECK_EQ(enc("foob"), "Zm9vYg==");
    CHECK_EQ(enc("fooba"), "Zm9vYmE=");
    CHECK_EQ(enc("foobar"), "Zm9vYmFy");
}

TEST(color_ids) {
    CHECK_EQ(color_id_from_value(Value::str("Red")), -7);
    CHECK_EQ(color_id_from_value(Value::str("transparent")), -44);
    CHECK_EQ(color_id_from_value(Value::str("042")), 42);
    CHECK_EQ(color_id_from_value(Value::real(49.5)), 50);
    CHECK_EQ(color_id_from_value(Value::real(48.5)), 48);
    CHECK_EQ(color_id_from_value(Value::real(150.0)), 99);
    CHECK_THROWS(color_id_from_value(Value::integer(100)));
    CHECK_THROWS(color_id_from_value(Value::str("0000000000000000000000150")));
    CHECK_THROWS(color_id_from_value(Value::str("nope")));
}

TEST(draw_batch_failed_append_leaves_buffer_intact) {
    DrawBatch batch;
    CHECK(batch.append("plot", {Value::integer(1), Value::integer(2), Value::str("red")}));
    auto before = batch.bytes();
    CHECK_THROWS(batch.append("draw_circle", {Value::integer(32), Value::integer(32), Value::integer(5),
                                              Value::str("red"), Value::integer(100), Value::boolean(false),
                                              Value::none(), Value::str("bogus")}));
    CHECK(batch.bytes() == before);
    CHECK_EQ(batch.record_count(), 1u);
}

TEST(draw_batch_goldens) {
    std::ifstream in(golden_path());
    if (!in) {
        ::pixil_test::report(__FILE__, __LINE__, std::string("missing golden file ") + golden_path());
        return;
    }
    DrawBatch batch;
    std::string line;
    int cases = 0;
    bool saw_batch = false;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json::Node rec = json::parse(line);
        const std::string id = rec.get("id")->s;
        if (id == "batch") {
            saw_batch = true;
            std::string b64 = base64_encode(batch.bytes().data(), batch.bytes().size());
            if (b64 != rec.get("b64")->s) ::pixil_test::report(__FILE__, __LINE__, "concatenated batch base64 mismatch");
            continue;
        }
        ++cases;
        std::vector<Value> args;
        for (const auto& a : rec.get("args")->items) args.push_back(from_json(a));
        const std::string cmd = rec.get("cmd")->s;
        const size_t before = batch.bytes().size();
        const json::Node* hex = rec.get("hex");
        try {
            batch.append(cmd, args);
            if (!hex) {
                ::pixil_test::report(__FILE__, __LINE__, id + ": legacy raised " + rec.get("error")->s +
                                                             ", native packed " + to_hex(batch.bytes(), before));
            } else if (to_hex(batch.bytes(), before) != hex->s) {
                ::pixil_test::report(__FILE__, __LINE__, id + ": legacy " + hex->s + " native " +
                                                             to_hex(batch.bytes(), before));
            }
        } catch (const std::exception& e) {
            if (hex) ::pixil_test::report(__FILE__, __LINE__, id + ": native threw " + e.what() + ", legacy " + hex->s);
        }
    }
    CHECK(saw_batch);
    CHECK(cases > 100);
}
