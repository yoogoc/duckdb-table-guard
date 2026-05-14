#define DUCKDB_EXTENSION_MAIN

#include "table_guard_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/printer.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
// AllowEntry: catalog.schema.table triple
// ---------------------------------------------------------------------------

struct AllowEntry {
	string catalog;
	string schema;
	string table;

	bool matches(const string &c, const string &s, const string &t) const {
		auto wild = [](const string &pattern, const string &val) -> bool {
			return pattern.empty() || pattern == val;
		};
		return wild(catalog, c) && wild(schema, s) && wild(table, t);
	}

	string str() const {
		auto fmt = [](const string &s) {
			return s.empty() ? "*" : s;
		};
		return fmt(catalog) + "." + fmt(schema) + "." + fmt(table);
	}
};

// ---------------------------------------------------------------------------
// Parse a single allowlist entry string
// ---------------------------------------------------------------------------

static string TrimString(const string &raw) {
	string s = raw;
	StringUtil::Trim(s);
	return s;
}

static AllowEntry ParseAllowEntry(const string &raw) {
	string trimmed = TrimString(raw);
	auto parts = StringUtil::Split(trimmed, '.');

	auto norm = [](const string &s) -> string {
		return (s == "*") ? "" : s;
	};

	AllowEntry e;
	if (parts.size() >= 3) {
		e.catalog = norm(parts[0]);
		e.schema = norm(parts[1]);
		e.table = norm(parts[2]);
	} else if (parts.size() == 2) {
		e.catalog = "";
		e.schema = norm(parts[0]);
		e.table = norm(parts[1]);
	} else if (parts.size() == 1 && !trimmed.empty()) {
		e.catalog = "";
		e.schema = "";
		e.table = norm(parts[0]);
	}
	return e;
}

// ---------------------------------------------------------------------------
// TableGuardState: per-connection state on ClientContext::registered_state
// ---------------------------------------------------------------------------

static const char *GUARD_KEY = "table_guard";

struct TableGuardState : public ClientContextState {
	bool enabled = false;
	bool configured = false;
	vector<AllowEntry> allowed;

	void QueryEnd() override {
	}

	string AllowedStr() const {
		vector<string> parts;
		parts.reserve(allowed.size());
		for (auto &e : allowed) {
			parts.push_back(e.str());
		}
		return "[" + StringUtil::Join(parts, ", ") + "]";
	}
};

static shared_ptr<TableGuardState> GetGuardState(ClientContext &ctx) {
	return ctx.registered_state->Get<TableGuardState>(GUARD_KEY);
}

static shared_ptr<TableGuardState> EnsureGuardState(ClientContext &ctx) {
	return ctx.registered_state->GetOrCreate<TableGuardState>(GUARD_KEY);
}

// ---------------------------------------------------------------------------
// Optimizer Pass: walk the logical plan tree, check all base table scans
// ---------------------------------------------------------------------------

static bool ExtractTableIdentifier(const LogicalGet &get, string &out_catalog, string &out_schema, string &out_table) {
	if (get.function.name != "seq_scan" || !get.bind_data) {
		return false;
	}

	auto &scan_data = get.bind_data->Cast<TableScanBindData>();
	auto &entry = scan_data.table;

	out_catalog = entry.schema.catalog.GetName();
	out_schema = entry.schema.name;
	out_table = entry.name;
	return true;
}

// ---------------------------------------------------------------------------
// Metadata filtering: inject LogicalFilter on duckdb_tables/views/columns
// ---------------------------------------------------------------------------

static const vector<string> METADATA_FUNCTIONS = {"duckdb_tables", "duckdb_views", "duckdb_columns"};

struct MetadataColumnPositions {
	idx_t database_pos;
	idx_t schema_pos;
	idx_t table_pos;
	bool found;
};

static MetadataColumnPositions FindMetadataColumns(LogicalGet &get) {
	MetadataColumnPositions result;
	result.found = false;

	const string &func_name = get.function.name;
	string table_col_name;
	if (func_name == "duckdb_tables") {
		table_col_name = "table_name";
	} else if (func_name == "duckdb_views") {
		table_col_name = "view_name";
	} else if (func_name == "duckdb_columns") {
		table_col_name = "table_name";
	} else {
		return result;
	}

	idx_t db_orig = DConstants::INVALID_INDEX;
	idx_t schema_orig = DConstants::INVALID_INDEX;
	idx_t table_orig = DConstants::INVALID_INDEX;

	for (idx_t i = 0; i < get.names.size(); i++) {
		if (get.names[i] == "database_name") {
			db_orig = i;
		} else if (get.names[i] == "schema_name") {
			schema_orig = i;
		} else if (get.names[i] == table_col_name) {
			table_orig = i;
		}
	}

	if (db_orig == DConstants::INVALID_INDEX || schema_orig == DConstants::INVALID_INDEX ||
	    table_orig == DConstants::INVALID_INDEX) {
		return result;
	}

	auto find_or_add = [&](idx_t orig_col_idx) -> idx_t {
		auto &col_ids = get.GetColumnIds();
		for (idx_t i = 0; i < col_ids.size(); i++) {
			if (col_ids[i].HasPrimaryIndex() && col_ids[i].GetPrimaryIndex() == orig_col_idx) {
				return i;
			}
		}
		idx_t new_pos = col_ids.size();
		get.AddColumnId(orig_col_idx);
		return new_pos;
	};

	result.database_pos = find_or_add(db_orig);
	result.schema_pos = find_or_add(schema_orig);
	result.table_pos = find_or_add(table_orig);
	result.found = true;
	return result;
}

static unique_ptr<Expression> MakeEqualityFilter(idx_t table_index, idx_t col_pos, const string &value) {
	auto col_ref = make_uniq<BoundColumnRefExpression>(LogicalType::VARCHAR, ColumnBinding(table_index, col_pos));
	auto constant = make_uniq<BoundConstantExpression>(Value(value));
	return make_uniq<BoundComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(col_ref), std::move(constant));
}

static unique_ptr<Expression> BuildAllowlistFilter(const TableGuardState &guard, idx_t table_index,
                                                   const MetadataColumnPositions &cols) {
	unique_ptr<Expression> combined;

	for (const auto &entry : guard.allowed) {
		vector<unique_ptr<Expression>> conditions;

		if (!entry.catalog.empty()) {
			conditions.push_back(MakeEqualityFilter(table_index, cols.database_pos, entry.catalog));
		}
		if (!entry.schema.empty()) {
			conditions.push_back(MakeEqualityFilter(table_index, cols.schema_pos, entry.schema));
		}
		if (!entry.table.empty()) {
			conditions.push_back(MakeEqualityFilter(table_index, cols.table_pos, entry.table));
		}

		unique_ptr<Expression> entry_filter;
		if (conditions.empty()) {
			continue;
		} else if (conditions.size() == 1) {
			entry_filter = std::move(conditions[0]);
		} else {
			auto conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
			for (auto &cond : conditions) {
				conjunction->children.push_back(std::move(cond));
			}
			entry_filter = std::move(conjunction);
		}

		if (!combined) {
			combined = std::move(entry_filter);
		} else {
			auto disjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR);
			disjunction->children.push_back(std::move(combined));
			disjunction->children.push_back(std::move(entry_filter));
			combined = std::move(disjunction);
		}
	}

	return combined;
}

// ---------------------------------------------------------------------------
// WalkPlan: recursive plan tree walker
// ---------------------------------------------------------------------------

static void WalkPlan(unique_ptr<LogicalOperator> &op_ptr, const TableGuardState &guard) {
	auto &op = *op_ptr;

	for (auto &child : op.children) {
		WalkPlan(child, guard);
	}

	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();

		string catalog, schema, table;
		if (ExtractTableIdentifier(get, catalog, schema, table)) {
			bool ok = false;
			for (const auto &entry : guard.allowed) {
				if (entry.matches(catalog, schema, table)) {
					ok = true;
					break;
				}
			}

			if (!ok) {
				throw PermissionException("TableGuard: \"%s.%s.%s\" is not in the allowlist.\n"
				                          "  Allowed: %s",
				                          catalog, schema, table, guard.AllowedStr());
			}
			return;
		}

		for (const auto &meta_func : METADATA_FUNCTIONS) {
			if (get.function.name == meta_func) {
				auto cols = FindMetadataColumns(get);
				if (!cols.found) {
					throw PermissionException("TableGuard: metadata query \"%s\" blocked "
					                          "(required columns not projected).",
					                          meta_func);
				}

				auto filter_expr = BuildAllowlistFilter(guard, get.table_index, cols);
				if (filter_expr) {
					auto filter_node = make_uniq<LogicalFilter>(std::move(filter_expr));
					filter_node->children.push_back(std::move(op_ptr));
					op_ptr = std::move(filter_node);
				}
				return;
			}
		}
	}
}

struct TableGuardOptimizer : OptimizerExtension {
	TableGuardOptimizer() {
		optimize_function = Run;
	}

	static void Run(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
		auto guard = GetGuardState(input.context);
		if (!guard || !guard->enabled) {
			return;
		}
		WalkPlan(plan, *guard);
	}
};

// ---------------------------------------------------------------------------
// Pragma management interface
// ---------------------------------------------------------------------------

static void PragmaAllow(ClientContext &ctx, const FunctionParameters &params) {
	auto guard = EnsureGuardState(ctx);

	if (guard->configured) {
		throw PermissionException("TableGuard: allowlist has already been configured for this connection "
		                          "and cannot be changed.");
	}

	string raw = params.values[0].GetValue<string>();
	if (raw.empty()) {
		throw InvalidInputException("TableGuard: table_guard_allow requires a non-empty argument.\n"
		                            "  Example: PRAGMA table_guard_allow('mydb.public.patients, mydb.public.visits');");
	}

	for (auto &part : StringUtil::Split(raw, ',')) {
		string trimmed = TrimString(part);
		if (!trimmed.empty()) {
			guard->allowed.push_back(ParseAllowEntry(trimmed));
		}
	}

	if (guard->allowed.empty()) {
		throw InvalidInputException("TableGuard: no valid entries found in the allowlist string: '%s'", raw);
	}

	guard->configured = true;
	guard->enabled = true;
}

static void PragmaEnable(ClientContext &ctx, const FunctionParameters &) {
	auto guard = EnsureGuardState(ctx);
	if (!guard->configured) {
		throw InvalidInputException("TableGuard: call PRAGMA table_guard_allow(...) before enabling.");
	}
	guard->enabled = true;
}

static void PragmaDisable(ClientContext &ctx, const FunctionParameters &) {
	auto guard = EnsureGuardState(ctx);
	guard->enabled = false;
}

static void PragmaStatus(ClientContext &ctx, const FunctionParameters &) {
	auto guard = GetGuardState(ctx);
	if (!guard) {
		Printer::Print("TableGuard: not initialized on this connection.");
		return;
	}
	Printer::PrintF("TableGuard: enabled=%s, configured=%s, allowlist=%s", guard->enabled ? "true" : "false",
	                guard->configured ? "true" : "false", guard->AllowedStr());
}

// ---------------------------------------------------------------------------
// Extension entry
// ---------------------------------------------------------------------------

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();

	auto &callback_manager = ExtensionCallbackManager::Get(db);
	callback_manager.Register(TableGuardOptimizer());

	loader.RegisterFunction(PragmaFunction::PragmaCall("table_guard_allow", PragmaAllow, {LogicalType::VARCHAR}));

	loader.RegisterFunction(PragmaFunction::PragmaStatement("table_guard_enable", PragmaEnable));

	loader.RegisterFunction(PragmaFunction::PragmaStatement("table_guard_disable", PragmaDisable));

	loader.RegisterFunction(PragmaFunction::PragmaStatement("table_guard_status", PragmaStatus));
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
