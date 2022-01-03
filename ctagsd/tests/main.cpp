#include "CTags.hpp"
#include "CompletionHelper.hpp"
#include "CxxCodeCompletion.hpp"
#include "CxxExpression.hpp"
#include "CxxScannerTokens.h"
#include "CxxTokenizer.h"
#include "CxxVariableScanner.h"
#include "LSPUtils.hpp"
#include "Settings.hpp"
#include "SimpleTokenizer.hpp"
#include "clFilesCollector.h"
#include "clTempFile.hpp"
#include "ctags_manager.h"
#include "fileutils.h"
#include "macros.h"
#include "strings.hpp"
#include "tags_storage_sqlite3.h"
#include "tester.hpp"

#include <iostream>
#include <stdio.h>
#include <wx/init.h>
#include <wx/log.h>
#include <wxStringHash.h>

using namespace std;
namespace
{
ostream& operator<<(ostream& stream, const vector<wxString>& arrstr)
{
    wxString s;
    s << "[";
    for(const auto& str : arrstr) {
        s << str << ",";
    }
    if(!s.empty()) {
        s.RemoveLast();
    }
    s << "]";
    stream << s;
    return stream;
}

ostream& operator<<(ostream& stream, const pair<vector<SimpleTokenizer::Token>, wxString>& pr)
{
    wxString s;
    SimpleTokenizer helper({});
    const vector<SimpleTokenizer::Token>& tokens = pr.first;
    const wxString& source_string = pr.second;
    for(const SimpleTokenizer::Token& token : tokens) {
        wxString as_str = token.to_string(source_string);
        helper.strip_comment(as_str);
        s << "Line: " << token.line() << "\n"
          << "Comment:\n"
          << as_str << "\n------------\n";
    }
    stream << s << endl;
    return stream;
}

thread_local bool cc_initialised = false;
thread_local bool cc_initialised_successfully = false;
thread_local ITagsStoragePtr lookup_table;
thread_local wxStringMap_t types_table;
thread_local wxStringMap_t macros_table;
thread_local CxxCodeCompletion::ptr_t completer;

CTagsdSettings settings;

#define ENSURE_DB_LOADED()                                                                                          \
    if(!initialize_cc_tests()) {                                                                                    \
        cout << "CC database not loaded. Please set environment variable TAGS_DB that points to `tags.db`" << endl; \
        return true;                                                                                                \
    }

bool initialize_cc_tests()
{
    if(!cc_initialised) {
        cc_initialised = true;
        wxString tags_db;
        if(!wxGetEnv("TAGS_DB", &tags_db)) {
            cc_initialised_successfully = false;
            wxPrintf("Loading TAGS_DB=%s\n", tags_db);
        } else {
            wxFileName fn(tags_db);
            wxFileName fn_settings(fn.GetPath(), "settings.json");
            settings.Load(fn_settings);

            lookup_table = ITagsStoragePtr(new TagsStorageSQLite());
            lookup_table->OpenDatabase(fn);

            types_table = settings.GetTypes();
            macros_table = settings.GetTokens();
            completer.reset(new CxxCodeCompletion(lookup_table));
            completer->set_macros_table({});

            // needed for unique_ptr
            completer->set_types_table({ { "_Ptr<_Tp,_Dp>::type", "_Tp" } });
            cc_initialised_successfully = true;
        }
    }
    return cc_initialised_successfully;
}
} // namespace

TEST_FUNC(TestLSPLocation)
{
    ENSURE_DB_LOADED();

    vector<TagEntryPtr> candidates;

    wxString code = "LSP::SymbolInformation si; si.GetLocation().";
    completer->set_text(code);
    TagEntryPtr resolved = completer->code_complete("si.GetLocation().", { "LSP" });
    if(resolved) {
        completer->get_completions(resolved, wxEmptyString, candidates, { "LSP" });
    }
    CHECK_BOOL(resolved);
    CHECK_SIZE(candidates.size(), 18);
    return true;
}

TEST_FUNC(TestCompletionHelper_get_expression)
{
    wxStringMap_t M = {
        { "m_string.", "m_string." },
        { "string name=m_string.", "m_string." },
        { "string name=m_string->", "m_string->" },
        { "get_details().get_name().", "get_details().get_name()." },
        { "foo(){ std::vector<std::string>::", "std::vector<std::string>::" },
        { "auto foo = [=](std::ve", "std::ve" },
        { "string name", "name" },
        { "if(!types.empty() && types.back() == T_IDENTIF", "T_IDENTIF" },
        { "if(!types.empty() && type", "type" },
        { "if(!typ", "typ" },
        { sample_text, "m_string." },
        { "for(const wxString& a: str", "str" },
        { "vector<wxString> split_function_signature", "split_function_signature" },
    };

    CompletionHelper helper;
    for(const auto& vt : M) {
        const wxString& raw_string = vt.first;
        const wxString& expected = vt.second;
        CHECK_STRING(helper.get_expression(raw_string, false), expected);
    }
    return true;
}

TEST_FUNC(TestSplitArgsNoArgumentName)
{
    wxString signature_1 = "(const LSP::ResponseMessage& response, wxEvtHandler* owner = nullptr)";
    wxString signature_2 =
        "(const LSP::ResponseMessage& response, wxEvtHandler* owner = nullptr, const vector<std::string>& response)";
    wxString signature_3 =
        "(const LSP::ResponseMessage& response, wxEvtHandler* owner = nullptr, vector<std::string>&& response)";
    CompletionHelper helper;

    wxString return_value;
    {
        auto args =
            helper.split_function_signature(signature_1, &return_value, CompletionHelper::STRIP_NO_DEFAULT_VALUES);
        CHECK_SIZE(args.size(), 2);
    }
    {
        auto args = helper.split_function_signature(
            signature_2, &return_value, CompletionHelper::STRIP_NO_DEFAULT_VALUES | CompletionHelper::STRIP_NO_NAME);
        CHECK_SIZE(args.size(), 3);
    }

    {
        auto args = helper.split_function_signature(
            signature_3, &return_value, CompletionHelper::STRIP_NO_DEFAULT_VALUES | CompletionHelper::STRIP_NO_NAME);
        CHECK_SIZE(args.size(), 3);
    }
    return true;
}

TEST_FUNC(TestCompletionHelper_truncate_file_to_location)
{
    CompletionHelper helper;
    wxString f = helper.truncate_file_to_location(file_content, 4, 6, CompletionHelper::TRUNCATE_EXACT_POS);
    auto lines = ::wxStringTokenize(f, "\n", wxTOKEN_RET_EMPTY_ALL);
    CHECK_BOOL(!lines.empty());
    CHECK_SIZE(lines.size(), 5);
    CHECK_STRING(lines[4].Trim().Trim(false), "V.");
    return true;
}

TEST_FUNC(TestCompletionHelper_truncate_file_to_location_invalid_input)
{
    wxString file_content_empty = "";

    CompletionHelper helper;
    wxString f = helper.truncate_file_to_location(file_content_empty, 4, 6, CompletionHelper::TRUNCATE_EXACT_POS);
    auto lines = ::wxStringTokenize(f, "\n", wxTOKEN_RET_EMPTY_ALL);
    CHECK_BOOL(lines.empty());
    CHECK_SIZE(lines.size(), 0);
    return true;
}

TEST_FUNC(TestCompletionHelper_truncate_file_to_location_must_end_with_words)
{
    wxString file_content_z = "std::vector<TagEntryPtr>";

    CompletionHelper helper;
    wxString f = helper.truncate_file_to_location(file_content_z, 0, 15, CompletionHelper::TRUNCATE_COMPLETE_WORDS);
    CHECK_STRING(f, "std::vector<TagEntryPtr");
    return true;
}

TEST_FUNC(TestCTagsManager_AutoCandidates)
{
    ENSURE_DB_LOADED();

    vector<TagEntryPtr> candidates;
    wxString fulltext = "wxCodeCompletionBoxManager::Get().";

    auto resolved = completer->code_complete(fulltext, {});
    CHECK_BOOL(resolved);

    completer->get_completions(resolved, wxEmptyString, candidates, {});
    CHECK_BOOL(!candidates.empty());
    return true;
}

TEST_FUNC(TestSimeplTokenizer_Comments)
{
    SimpleTokenizer::Token token;
    SimpleTokenizer tokenizer(tokenizer_sample_file_0);
    vector<SimpleTokenizer::Token> tokens;
    while(tokenizer.next_comment(&token)) {
        tokens.push_back(token);
    }
    // we are expecting 2 comments
    CHECK_SIZE(tokens.size(), 4);
    cout << make_pair(tokens, tokenizer_sample_file_0) << endl;
    return true;
}

TEST_FUNC(TestSimeplTokenizer_2)
{
    SimpleTokenizer::Token token;
    SimpleTokenizer tokenizer(sample_text_language_h);
    vector<SimpleTokenizer::Token> tokens;
    vector<TokenWrapper> tokens_vec;
    while(tokenizer.next(&token)) {
        tokens.push_back(token);
        TokenWrapper t;
        t.token = token;
        tokens_vec.push_back(t);
    }

    // collect all interesting tokens from the document
    vector<int> encoded_arr;
    LSPUtils::encode_semantic_tokens(tokens_vec, &encoded_arr);
    for(size_t i = 0; i < encoded_arr.size() / 5; ++i) {
        size_t base = i * 5;
        const auto& tok = tokens_vec[i].token;
        cout << tok.to_string(sample_text_language_h) << "(" << tok.line() << "," << tok.column() << "," << tok.length()
             << "): ";
        cout << encoded_arr[base + 0] << ", ";
        cout << encoded_arr[base + 1] << ", ";
        cout << encoded_arr[base + 2] << ", ";
        cout << encoded_arr[base + 3] << ", ";
        cout << encoded_arr[base + 4] << endl;
    }
    return true;
}

TEST_FUNC(TestSimeplTokenizer)
{
    {
        SimpleTokenizer::Token token;
        SimpleTokenizer tokenizer(tokenizer_sample_file_1);
        vector<SimpleTokenizer::Token> tokens;
        while(tokenizer.next(&token)) {
            tokens.push_back(token);
        }
        CHECK_SIZE(tokens.size(), 15);
    }
    return true;
}

TEST_FUNC(TestSplitArgs)
{
    const wxString signature_1 = "(const LSP::ResponseMessage& response, wxEvtHandler* owner)";
    const wxString signature_2 = "(int argc, char** argv) -> int";
    const wxString signature_3 = "void : (const wxString& name, std::function<void(const JSONItem& item)> "
                                 "deserialiser_func, const wxFileName& configFile = {})";
    const wxString signature_4 = "(const wxString& name, std::function<void(const JSONItem& item)> "
                                 "deserialiser_func, const wxFileName& configFile = {}, std::string value = "
                                 "\"default_string\") -> std::vector<int>::iterator*";
    const wxString signature_5 = "(unique_ptr<JSON>&& msg, Channel& channel) -> wxString::size_type";
    CompletionHelper helper;
    wxString return_value;

    {
        return_value.clear();
        auto args = helper.split_function_signature(signature_1, &return_value);
        CHECK_SIZE(args.size(), 2);
        CHECK_BOOL(return_value.empty());
        cout << args << endl;
    }

    {
        return_value.clear();
        auto args = helper.split_function_signature(signature_2, &return_value);
        CHECK_SIZE(args.size(), 2);
        CHECK_STRING(return_value, "int");
        cout << args << endl;
    }
    {
        return_value.clear();
        auto args = helper.split_function_signature(signature_3, &return_value);
        CHECK_SIZE(args.size(), 3);
        CHECK_BOOL(return_value.empty());
        cout << args << endl;
    }
    {
        return_value.clear();
        auto args = helper.split_function_signature(signature_4, &return_value);
        CHECK_SIZE(args.size(), 4);
        CHECK_STRING(return_value, "std::vector<int>::iterator*");
        cout << args << endl;
    }

    {
        return_value.clear();
        auto args = helper.split_function_signature(signature_5, &return_value);
        CHECK_SIZE(args.size(), 2);
        CHECK_STRING(return_value, "wxString::size_type");
        cout << args << endl;
    }

    return true;
}

TEST_FUNC(test_symlink_is_scandir)
{
    clFilesScanner scanner;
    wxArrayString files;
    scanner.Scan("/tmp/eran", files, "*.cpp", "", "");
    return true;
}

TEST_FUNC(test_cxx_expression)
{
    CxxExpression remainder;
    vector<CxxExpression> exp1 = CxxExpression::from_expression("std::vector<int>::", &remainder);
    CHECK_SIZE(exp1.size(), 2);
    CHECK_EXPECTED(exp1[0].type_name(), "std");
    CHECK_EXPECTED(exp1[0].operand_string(), "::");

    CHECK_EXPECTED(exp1[1].type_name(), "vector");
    CHECK_EXPECTED(exp1[1].operand_string(), "::");
    CHECK_EXPECTED(exp1[1].template_init_list().size(), 1);

    vector<CxxExpression> exp2 = CxxExpression::from_expression("string.AfterFirst('(').", &remainder);
    CHECK_SIZE(exp2.size(), 2);

    vector<CxxExpression> exp3 = CxxExpression::from_expression("string.AfterFirst('(')", &remainder);
    CHECK_SIZE(exp3.size(), 1);
    CHECK_EXPECTED(remainder.type_name(), "AfterFirst");
    return true;
}

TEST_FUNC(test_cxx_code_completion_this_and_global_scope)
{
    ENSURE_DB_LOADED();
    // use a line inside CxxCodeCompletion file
    wxString filename = R"(C:\src\codelite\CodeLite\ctags_manager.cpp)";

    TagEntryPtr resolved;
    if(wxFileExists(filename)) {
        completer->set_text(wxEmptyString, filename, 149);
        resolved = completer->code_complete("this->", {});
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "TagsManager");
    }

    resolved = completer->code_complete("::", {});
    CHECK_BOOL(resolved);
    CHECK_STRING(resolved->GetPath(), "<global>");

    CxxExpression remainder;
    resolved = completer->code_complete("wxS", {}, &remainder);
    CHECK_BOOL(resolved);
    CHECK_STRING(resolved->GetPath(), "<global>");
    CHECK_STRING(remainder.type_name(), "wxS");

    return true;
}

TEST_FUNC(test_cxx_code_completion_list_locals)
{
    ENSURE_DB_LOADED();
    {
        completer->set_text(tokenizer_sample_file_0);
        auto locals = completer->get_locals();
        CHECK_SIZE(locals.size(), 0);
    }

    {
        completer->set_text(tokenizer_sample_file_2);
        auto locals = completer->get_locals();
        CHECK_SIZE(locals.size(), 0);
    }

    {
        completer->set_text(file_content);
        auto locals = completer->get_locals();
        CHECK_SIZE(locals.size(), 1);
    }

    {
        completer->set_text(cc_text_auto_chained);
        auto locals = completer->get_locals();
        CHECK_SIZE(locals.size(), 2);
        CHECK_STRING(locals[0]->GetName(), "str");
        CHECK_STRING(locals[1]->GetName(), "arr");
    }

    {
        completer->set_text(big_file);
        auto locals = completer->get_locals();
        // we expect 4 variables
        CHECK_SIZE(locals.size(), 4);
    }

    {
        completer->set_text(cc_lamda_text);
        auto locals = completer->get_locals();
        CHECK_SIZE(locals.size(), 6);
    }

    return true;
}

TEST_FUNC(test_cxx_code_completion_function_arguments)
{
    ENSURE_DB_LOADED();
    {
        completer->set_text(cc_text_function_args_simple);
        TagEntryPtr resolved = completer->code_complete("str.", { "std" });
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "wxString");
    }
    {
        completer->set_text(cc_text_function_args_simple);
        TagEntryPtr resolved = completer->code_complete("json.", { "std" });
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "std::shared_ptr");
    }

    {
        completer->set_text(cc_text_function_args_simple);
        TagEntryPtr resolved = completer->code_complete("json->", { "std" });
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "JSON");
    }
    return true;
}

TEST_FUNC(test_cxx_code_completion_rvalue_reference)
{
    ENSURE_DB_LOADED();
    {
        wxString text = "TagEntry&& entry;";
        completer->set_text(text);
        TagEntryPtr resolved = completer->code_complete("entry.m_path.", {});
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "wxString");
    }
    return true;
}

TEST_FUNC(test_cxx_code_completion_member_variable)
{
    ENSURE_DB_LOADED();
    {
        wxString text = "TagEntry entry;";
        completer->set_text(text);
        TagEntryPtr resolved = completer->code_complete("entry.m_path.", {});
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "wxString");
    }
    return true;
}

TEST_FUNC(test_cxx_code_completion_template)
{
    ENSURE_DB_LOADED();

    {
        wxString text = "wxVector<wxString> V;";
        completer->set_text(text);
        TagEntryPtr resolved = completer->code_complete("V.at(0).", {});
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "wxString");
    }
    {
        // template inheritance in typedef
        // typedef Singleton<Manager> ManagerST;
        completer->reset();
        TagEntryPtr resolved = completer->code_complete("ManagerST::Get()->", {});
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "Manager");
    }

    {
        // template inheritance
        // class ContextManager : public Singleton<ContextManager> {...}
        // ContextManager::Get()->
        completer->reset();
        TagEntryPtr resolved = completer->code_complete("ContextManager::Get()->", {});
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "ContextManager");
    }

    {
        wxString text = "map<wxString, wxArrayString> M;";
        completer->set_text(text);
        TagEntryPtr resolved = completer->code_complete("M.at(str).", { "std" });
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "wxArrayString");
    }

    {
        wxString text = "shared_ptr<wxString> P;";
        completer->set_text(text);
        TagEntryPtr resolved = completer->code_complete("P.", { "std" });
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "std::shared_ptr");
    }

    {
        wxString text = "shared_ptr<wxString> P;";
        completer->set_text(text);
        TagEntryPtr resolved = completer->code_complete("P->", { "std" });
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "wxString");
    }
    {
        wxString text = "unique_ptr<wxString> P;";
        completer->set_text(text);
        TagEntryPtr resolved = completer->code_complete("P->", { "std" });
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "wxString");
    }
    {
        completer->set_text(cc_text_ProtocolHandler);
        TagEntryPtr resolved = completer->code_complete("msg->", { "std" });
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "JSON");
    }
    return true;
}

TEST_FUNC(test_cxx_code_completion)
{
    ENSURE_DB_LOADED();

    {
        wxString text = "std::string str;";
        completer->set_text(text);
        TagEntryPtr resolved = completer->code_complete("str.", {});
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "std::basic_string");
    }

    {
        completer->set_text(cc_text_auto_chained);
        TagEntryPtr resolved = completer->code_complete("str.", {});
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "wxString");
    }

    {
        completer->set_text(cc_text_auto_simple);
        TagEntryPtr resolved = completer->code_complete("str.", {});
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "wxString");
    }

    {
        completer->set_text(cc_text_simple);
        TagEntryPtr resolved = completer->code_complete("wxString::", {});
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "wxString");
    }

    {
        completer->set_text(cc_text_simple);
        TagEntryPtr resolved = completer->code_complete("str.AfterFirst('(').", {});
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "wxString");
    }

    {
        completer->set_text(cc_text_simple);
        TagEntryPtr resolved = completer->code_complete("str.AfterFirst('(').BeforeFirst().", {});
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "wxString");
    }

    {
        completer->set_text(cc_text_lsp_event);
        TagEntryPtr resolved = completer->code_complete("event.GetLocation().GetRange().GetStart().", { "LSP", "std" });
        CHECK_BOOL(resolved);
        CHECK_STRING(resolved->GetPath(), "LSP::Position");
    }
    return true;
}

int main(int argc, char** argv)
{
    wxInitializer initializer(argc, argv);
    wxLogNull NOLOG;
    Tester::Instance()->RunTests();
    return 0;
}
