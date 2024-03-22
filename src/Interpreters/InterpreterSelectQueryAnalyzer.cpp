#include <Interpreters/InterpreterFactory.h>
#include <Interpreters/InterpreterSelectQueryAnalyzer.h>

#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTSubquery.h>

#include <DataTypes/DataTypesNumber.h>

#include <Processors/QueryPlan/IQueryPlanStep.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>
#include <QueryPipeline/QueryPipelineBuilder.h>

#include <Storages/IStorage.h>

#include <Analyzer/QueryTreeBuilder.h>
#include <Analyzer/QueryTreePassManager.h>
#include <Analyzer/IdentifierNode.h>
#include <Analyzer/QueryNode.h>
#include <Analyzer/UnionNode.h>
#include <Analyzer/TableNode.h>
#include <Analyzer/TableFunctionNode.h>
#include <Analyzer/Utils.h>

#include <Interpreters/Context.h>
#include <Interpreters/QueryLog.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int UNSUPPORTED_METHOD;
}

namespace
{

ASTPtr normalizeAndValidateQuery(const ASTPtr & query)
{
    if (query->as<ASTSelectWithUnionQuery>() || query->as<ASTSelectQuery>())
    {
        return query;
    }
    else if (auto * subquery = query->as<ASTSubquery>())
    {
        return subquery->children[0];
    }
    else
    {
        throw Exception(ErrorCodes::UNSUPPORTED_METHOD,
            "Expected ASTSelectWithUnionQuery or ASTSelectQuery. Actual {}",
            query->formatForErrorMessage());
    }
}

ContextMutablePtr buildContext(const ContextPtr & context, const SelectQueryOptions & select_query_options)
{
    auto result_context = Context::createCopy(context);

    if (select_query_options.shard_num)
        result_context->addSpecialScalar(
            "_shard_num",
            Block{{DataTypeUInt32().createColumnConst(1, *select_query_options.shard_num), std::make_shared<DataTypeUInt32>(), "_shard_num"}});
    if (select_query_options.shard_count)
        result_context->addSpecialScalar(
            "_shard_count",
            Block{{DataTypeUInt32().createColumnConst(1, *select_query_options.shard_count), std::make_shared<DataTypeUInt32>(), "_shard_count"}});

    return result_context;
}

void replaceStorageInQueryTree(QueryTreeNodePtr & query_tree, const ContextPtr & context, const StoragePtr & storage)
{
    auto nodes = extractAllTableReferences(query_tree);
    IQueryTreeNode::ReplacementMap replacement_map;

    for (auto & node : nodes)
    {
        auto & table_node = node->as<TableNode &>();

        /// Don't replace storage if table name differs
        if (table_node.getStorageID().getFullNameNotQuoted() != storage->getStorageID().getFullNameNotQuoted())
            continue;

        auto replacement_table_expression = std::make_shared<TableNode>(storage, context);
        replacement_table_expression->setAlias(node->getAlias());

        if (auto table_expression_modifiers = table_node.getTableExpressionModifiers())
            replacement_table_expression->setTableExpressionModifiers(*table_expression_modifiers);

        replacement_map.emplace(node.get(), std::move(replacement_table_expression));
    }
    query_tree = query_tree->cloneAndReplace(replacement_map);
}

QueryTreeNodePtr buildQueryTreeAndRunPasses(const ASTPtr & query,
    const SelectQueryOptions & select_query_options,
    const ContextPtr & context,
    const StoragePtr & storage)
{
    auto query_tree = buildQueryTree(query, context);

    QueryTreePassManager query_tree_pass_manager(context);
    addQueryTreePasses(query_tree_pass_manager, select_query_options.only_analyze);

    /// We should not apply any query tree level optimizations on shards
    /// because it can lead to a changed header.
    if (select_query_options.ignore_ast_optimizations
        || context->getClientInfo().query_kind == ClientInfo::QueryKind::SECONDARY_QUERY)
        query_tree_pass_manager.runOnlyResolve(query_tree);
    else
        query_tree_pass_manager.run(query_tree);

    if (storage)
        replaceStorageInQueryTree(query_tree, context, storage);

    return query_tree;
}

}

InterpreterSelectQueryAnalyzer::InterpreterSelectQueryAnalyzer(
    const ASTPtr & query_,
    const ContextPtr & context_,
    const SelectQueryOptions & select_query_options_)
    : query(normalizeAndValidateQuery(query_))
    , context(buildContext(context_, select_query_options_))
    , select_query_options(select_query_options_)
    , query_tree(buildQueryTreeAndRunPasses(query, select_query_options, context, nullptr /*storage*/))
    , planner(query_tree, select_query_options)
{
}

InterpreterSelectQueryAnalyzer::InterpreterSelectQueryAnalyzer(
    const ASTPtr & query_,
    const ContextPtr & context_,
    const StoragePtr & storage_,
    const SelectQueryOptions & select_query_options_)
    : query(normalizeAndValidateQuery(query_))
    , context(buildContext(context_, select_query_options_))
    , select_query_options(select_query_options_)
    , query_tree(buildQueryTreeAndRunPasses(query, select_query_options, context, storage_))
    , planner(query_tree, select_query_options)
{
}

InterpreterSelectQueryAnalyzer::InterpreterSelectQueryAnalyzer(
    const QueryTreeNodePtr & query_tree_,
    const ContextPtr & context_,
    const SelectQueryOptions & select_query_options_)
    : query(query_tree_->toAST())
    , context(buildContext(context_, select_query_options_))
    , select_query_options(select_query_options_)
    , query_tree(query_tree_)
    , planner(query_tree_, select_query_options)
{
}

Block InterpreterSelectQueryAnalyzer::getSampleBlock(const ASTPtr & query,
    const ContextPtr & context,
    const SelectQueryOptions & select_query_options)
{
    auto select_query_options_copy = select_query_options;
    select_query_options_copy.only_analyze = true;
    InterpreterSelectQueryAnalyzer interpreter(query, context, select_query_options_copy);

    return interpreter.getSampleBlock();
}

Block InterpreterSelectQueryAnalyzer::getSampleBlock(const QueryTreeNodePtr & query_tree,
    const ContextPtr & context,
    const SelectQueryOptions & select_query_options)
{
    auto select_query_options_copy = select_query_options;
    select_query_options_copy.only_analyze = true;
    InterpreterSelectQueryAnalyzer interpreter(query_tree, context, select_query_options_copy);

    return interpreter.getSampleBlock();
}

Block InterpreterSelectQueryAnalyzer::getSampleBlock()
{
    planner.buildQueryPlanIfNeeded();
    return planner.getQueryPlan().getCurrentDataStream().header;
}

BlockIO InterpreterSelectQueryAnalyzer::execute()
{
    auto pipeline_builder = buildQueryPipeline();

    BlockIO result;
    result.pipeline = QueryPipelineBuilder::getPipeline(std::move(pipeline_builder));

    if (!select_query_options.ignore_quota && select_query_options.to_stage == QueryProcessingStage::Complete)
        result.pipeline.setQuota(context->getQuota());

    return result;
}

QueryPlan & InterpreterSelectQueryAnalyzer::getQueryPlan()
{
    planner.buildQueryPlanIfNeeded();
    return planner.getQueryPlan();
}

QueryPlan && InterpreterSelectQueryAnalyzer::extractQueryPlan() &&
{
    planner.buildQueryPlanIfNeeded();
    return std::move(planner).extractQueryPlan();
}

QueryPipelineBuilder InterpreterSelectQueryAnalyzer::buildQueryPipeline()
{
    planner.buildQueryPlanIfNeeded();
    auto & query_plan = planner.getQueryPlan();

    auto optimization_settings = QueryPlanOptimizationSettings::fromContext(context);
    auto build_pipeline_settings = BuildQueryPipelineSettings::fromContext(context);

    return std::move(*query_plan.buildQueryPipeline(optimization_settings, build_pipeline_settings));
}

void InterpreterSelectQueryAnalyzer::addStorageLimits(const StorageLimitsList & storage_limits)
{
    planner.addStorageLimits(storage_limits);
}

void InterpreterSelectQueryAnalyzer::extendQueryLogElemImpl(QueryLogElement & elem, const ASTPtr & /*ast*/, ContextPtr /*context*/) const
{
    for (const auto & used_row_policy : planner.getUsedRowPolicies())
        elem.used_row_policies.emplace(used_row_policy);
}

void registerInterpreterSelectQueryAnalyzer(InterpreterFactory & factory)
{
    auto create_fn = [] (const InterpreterFactory::Arguments & args)
    {
        return std::make_unique<InterpreterSelectQueryAnalyzer>(args.query, args.context, args.options);
    };
    factory.registerInterpreter("InterpreterSelectQueryAnalyzer", create_fn);
}

}
