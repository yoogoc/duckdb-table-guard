#define DUCKDB_EXTENSION_MAIN

#include "table_guard_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

namespace duckdb {

inline void TableGuardScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "...........🦆 " + name.GetString());
	});
}

inline void TableGuardOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "TableGuard " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto table_guard_scalar_function =
	    ScalarFunction("table_guard", {LogicalType::VARCHAR}, LogicalType::VARCHAR, TableGuardScalarFun);

	loader.RegisterFunction(table_guard_scalar_function);

	// Register another scalar function
	auto table_guard_openssl_version_scalar_function = ScalarFunction("table_guard_openssl_version", {LogicalType::VARCHAR},
	                                                             LogicalType::VARCHAR, TableGuardOpenSSLVersionScalarFun);
	loader.RegisterFunction(table_guard_openssl_version_scalar_function);
}

void TableGuardExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string TableGuardExtension::Name() {
	return "table_guard";
}

std::string TableGuardExtension::Version() const {
#ifdef EXT_VERSION_TABLE_GUARD
	return EXT_VERSION_TABLE_GUARD;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(table_guard, loader) {
	duckdb::LoadInternal(loader);
}
}
