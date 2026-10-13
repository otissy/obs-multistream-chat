// Small strict JSON value, parser and writer. Enough for the config file and the
// overlay feed; deliberately not a general library.
//   - parse: RFC 8259 only (no comments, no trailing commas, no NaN), depth and size limited
//   - objects keep insertion order; a duplicate key replaces the earlier value
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace oms {
namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
	Value() : type_(Type::Null), b_(false), n_(0) {}

	static Value boolean(bool b)
	{
		Value v;
		v.type_ = Type::Bool;
		v.b_ = b;
		return v;
	}
	static Value number(double n)
	{
		Value v;
		v.type_ = Type::Number;
		v.n_ = n;
		return v;
	}
	static Value string(const std::string &s)
	{
		Value v;
		v.type_ = Type::String;
		v.s_ = s;
		return v;
	}
	static Value array()
	{
		Value v;
		v.type_ = Type::Array;
		return v;
	}
	static Value object()
	{
		Value v;
		v.type_ = Type::Object;
		return v;
	}

	Type type() const { return type_; }
	bool is_null() const { return type_ == Type::Null; }
	bool is_bool() const { return type_ == Type::Bool; }
	bool is_number() const { return type_ == Type::Number; }
	bool is_string() const { return type_ == Type::String; }
	bool is_array() const { return type_ == Type::Array; }
	bool is_object() const { return type_ == Type::Object; }

	bool as_bool(bool def = false) const { return type_ == Type::Bool ? b_ : def; }
	double as_number(double def = 0) const { return type_ == Type::Number ? n_ : def; }
	int64_t as_int(int64_t def = 0) const { return type_ == Type::Number ? static_cast<int64_t>(n_) : def; }
	std::string as_string(const std::string &def = std::string()) const
	{
		return type_ == Type::String ? s_ : def;
	}

	// array access
	size_t size() const { return type_ == Type::Array ? items_.size() : type_ == Type::Object ? keys_.size() : 0; }
	const Value &at(size_t i) const { return items_[i]; }
	void push(const Value &v)
	{
		if (type_ == Type::Array)
			items_.push_back(v);
	}

	// object access
	const Value *find(const std::string &key) const
	{
		if (type_ != Type::Object)
			return nullptr;
		for (size_t i = 0; i < keys_.size(); ++i)
			if (keys_[i] == key)
				return &items_[i];
		return nullptr;
	}
	const std::string &key_at(size_t i) const { return keys_[i]; }
	const Value &value_at(size_t i) const { return items_[i]; }
	void set(const std::string &key, const Value &v)
	{
		if (type_ != Type::Object)
			return;
		for (size_t i = 0; i < keys_.size(); ++i) {
			if (keys_[i] == key) {
				items_[i] = v;
				return;
			}
		}
		keys_.push_back(key);
		items_.push_back(v);
	}

private:
	Type type_;
	bool b_;
	double n_;
	std::string s_;
	std::vector<Value> items_; // array items, or object values (parallel to keys_)
	std::vector<std::string> keys_;
};

struct ParseResult {
	bool ok = false;
	std::string error; // human readable, never contains input text beyond a position
	Value value;
};

ParseResult parse(const std::string &text, size_t max_bytes = (1u << 20), int max_depth = 32);

// Compact when pretty == false; two-space indent when true.
std::string dump(const Value &v, bool pretty = false);

// Escapes a string for use inside a JSON string literal (without the quotes).
std::string escape_string(const std::string &s);

} // namespace json
} // namespace oms
