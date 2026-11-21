// The mask of the script blocks in a markup text (hot::mask_embedded_scripts).

#include "hot/embedded_script.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <string>
#include <vector>

namespace
{

std::string sp(size_t n)
{
    return std::string(n, ' ');
}

size_t lines(const std::string& s)
{
    return static_cast<size_t>(std::count(s.begin(), s.end(), '\n'));
}

TEST(EmbeddedScript, NoBlock)
{
    bool found = true;
    EXPECT_EQ(hot::mask_embedded_scripts("<html>\n<body></body>\n</html>\n", "script", &found), "");
    EXPECT_FALSE(found);
    // The prefix does not make a result without a block.
    EXPECT_EQ(hot::mask_embedded_scripts("<p>x</p>", "script", &found, "P;"), "");
    EXPECT_FALSE(found);
}

TEST(EmbeddedScript, TwoBlocks)
{
    bool found = false;
    const std::string text = "<head>\n<script>\nint a;\n</script>\n<p>x</p>\n<script>\nint b;\n</script>\n</head>";
    EXPECT_EQ(hot::mask_embedded_scripts(text, "script", &found),
        "\n" + sp(8) + "\nint a;\n\n\n" + sp(8) + "\nint b;\n");
    EXPECT_TRUE(found);
}

TEST(EmbeddedScript, BlockOnTagLine)
{
    EXPECT_EQ(hot::mask_embedded_scripts("<div><script>int a;</script></div>"), sp(13) + "int a;");
    EXPECT_EQ(hot::mask_embedded_scripts("<p>\n  <script>int a;\nint b;</script>"), "\n" + sp(10) + "int a;\nint b;");
}

TEST(EmbeddedScript, TwoBlocksOnOneLine)
{
    EXPECT_EQ(hot::mask_embedded_scripts("<script>a</script><script>b</script>"), sp(8) + "a" + sp(17) + "b");
}

TEST(EmbeddedScript, CommentHoldsTag)
{
    bool found = false;
    EXPECT_EQ(hot::mask_embedded_scripts("<!-- <script>bad</script> -->\n<script>int a;</script>", "script", &found),
        "\n" + sp(8) + "int a;");
    EXPECT_TRUE(found);
    EXPECT_EQ(hot::mask_embedded_scripts("<!-- <script>bad</script>\n", "script", &found), "");
    EXPECT_FALSE(found);
    // A multi-line comment keeps the line count.
    EXPECT_EQ(hot::mask_embedded_scripts("<!--\n<script>x</script>\n-->\n<script>c</script>"), "\n\n\n" + sp(8) + "c");
}

TEST(EmbeddedScript, CdataOutsideBlock)
{
    bool found = true;
    EXPECT_EQ(hot::mask_embedded_scripts("<![CDATA[ <script>x</script> ]]>", "script", &found), "");
    EXPECT_FALSE(found);
}

TEST(EmbeddedScript, CdataInsideBlockIsCode)
{
    EXPECT_EQ(hot::mask_embedded_scripts("<script><![CDATA[a<b]]></script>"), sp(8) + "<![CDATA[a<b]]>");
}

TEST(EmbeddedScript, TagInAttributeOrString)
{
    bool found = true;
    EXPECT_EQ(hot::mask_embedded_scripts("<div title=\"<script>x</script>\">\n</div>", "script", &found), "");
    EXPECT_FALSE(found);
    const std::string text = "<a title=\"a>b\" href='<script>'>\n<script>c</script>";
    EXPECT_EQ(hot::mask_embedded_scripts(text), "\n" + sp(8) + "c");
    // A close tag in a string ends the block (as in the RmlUi parser).
    EXPECT_EQ(hot::mask_embedded_scripts("<script>s = \"</script>\";</script>"), sp(8) + "s = \"");
    // Another tag with the same start is not a block.
    EXPECT_EQ(hot::mask_embedded_scripts("<scripts>x</scripts>", "script", &found), "");
    EXPECT_FALSE(found);
}

TEST(EmbeddedScript, CrLf)
{
    EXPECT_EQ(hot::mask_embedded_scripts("<script>\r\nint a;\r\n</script>\r\n<p/>\r\n"), sp(8) + "\r\nint a;\r\n");
    EXPECT_EQ(hot::mask_embedded_scripts("<p>\r\n<script>a</script>\r\n<script>\r\nb</script>"),
        "\r\n" + sp(8) + "a\r\n" + sp(8) + "\r\nb");
}

TEST(EmbeddedScript, NoTrailingNewline)
{
    EXPECT_EQ(hot::mask_embedded_scripts("<script>x</script>"), sp(8) + "x");
    EXPECT_EQ(hot::mask_embedded_scripts("<script>x\n</script>"), sp(8) + "x\n");
}

TEST(EmbeddedScript, TextAfterLastBlockIsDropped)
{
    EXPECT_EQ(hot::mask_embedded_scripts("<script>x</script>\n\n<p>\n</p>\n"), sp(8) + "x");
}

TEST(EmbeddedScript, UnterminatedBlock)
{
    bool found = false;
    EXPECT_EQ(hot::mask_embedded_scripts("<script>\nint a;\n", "script", &found), sp(8) + "\nint a;\n");
    EXPECT_TRUE(found);
    EXPECT_EQ(hot::mask_embedded_scripts("<p>\n<script>a</scrip", "script", &found), "\n" + sp(8) + "a</scrip");
    EXPECT_TRUE(found);
    // An open tag without ">" is no block.
    EXPECT_EQ(hot::mask_embedded_scripts("<script src=\"a", "script", &found), "");
    EXPECT_FALSE(found);
}

TEST(EmbeddedScript, TagNameCase)
{
    EXPECT_EQ(hot::mask_embedded_scripts("<SCRIPT>a</Script>"), sp(8) + "a");
    EXPECT_EQ(hot::mask_embedded_scripts("<Script>a</ SCRIPT >"), sp(8) + "a");
    EXPECT_EQ(hot::mask_embedded_scripts("<script>a</\tscript\n>"), sp(8) + "a");
    EXPECT_EQ(hot::mask_embedded_scripts("<script>a</script>", "SCRIPT"), sp(8) + "a");
}

TEST(EmbeddedScript, EndTagOnTwoLinesKeepsLines)
{
    EXPECT_EQ(hot::mask_embedded_scripts("<script>a</script\n>\n<script>b</script>"), sp(8) + "a\n\n" + sp(8) + "b");
}

TEST(EmbeddedScript, Prefix)
{
    EXPECT_EQ(hot::mask_embedded_scripts("<script>\nint a;\n</script>", "script", nullptr, "P;"),
        "P;" + sp(8) + "\nint a;\n");
    EXPECT_EQ(hot::mask_embedded_scripts("<p>\n<script>a</script>", "script", nullptr, "P;"), "P;\n" + sp(8) + "a");
    // Line 2 and later keep the line and the column.
    const std::string out = hot::mask_embedded_scripts("<p>\n  <script>x\n  y</script>", "script", nullptr, "P;");
    EXPECT_EQ(out, "P;\n" + sp(10) + "x\n  y");
}

TEST(EmbeddedScript, EmptyInput)
{
    bool found = true;
    EXPECT_EQ(hot::mask_embedded_scripts("", "script", &found), "");
    EXPECT_FALSE(found);
    EXPECT_EQ(hot::mask_embedded_scripts("", "script", &found, "P;"), "");
    EXPECT_FALSE(found);
    EXPECT_EQ(hot::mask_embedded_scripts("<"), "");
    EXPECT_EQ(hot::mask_embedded_scripts("<script"), "");
}

TEST(EmbeddedScript, FoundFlag)
{
    bool found = false;
    hot::mask_embedded_scripts("<script>a</script>", "script", &found);
    EXPECT_TRUE(found);
    hot::mask_embedded_scripts("<p></p>", "script", &found);
    EXPECT_FALSE(found);
    // The pointer is optional.
    EXPECT_EQ(hot::mask_embedded_scripts("<script>a</script>"), sp(8) + "a");
}

TEST(EmbeddedScript, NonAscii)
{
    // "\xC3\xA9" is one letter with 2 bytes. The columns count bytes.
    const std::string e = "\xC3\xA9";
    EXPECT_EQ(hot::mask_embedded_scripts("<p>" + e + "</p><script>" + e + "</script>"), sp(3 + 2 + 4 + 8) + e);
    EXPECT_EQ(hot::mask_embedded_scripts("<script>\"" + e + "\"\n" + e + "</script>"), sp(8) + "\"" + e + "\"\n" + e);
}

TEST(EmbeddedScript, SrcAttribute)
{
    bool found = false;
    EXPECT_EQ(hot::mask_embedded_scripts("<script src=\"a.hot\"></script>", "script", &found), "");
    EXPECT_TRUE(found);
    EXPECT_EQ(hot::mask_embedded_scripts("<script src='a.hot'/>\n", "script", &found), "");
    EXPECT_TRUE(found);
    // Code after an empty block keeps its line.
    EXPECT_EQ(hot::mask_embedded_scripts("<script src=\"a>b\">\n</script>\n<script>c</script>"), sp(18) + "\n\n" + sp(8) + "c");
    const std::string text = "<script src=\"a.hot\">int a;</script>";
    EXPECT_EQ(hot::mask_embedded_scripts(text), sp(20) + "int a;");
}

TEST(EmbeddedScript, TextAfterHead)
{
    bool found = false;
    EXPECT_EQ(hot::mask_embedded_scripts("<head><script>a</script></head><body><script>b</script></body>", "script",
                  &found),
        sp(14) + "a");
    EXPECT_TRUE(found);
    EXPECT_EQ(hot::mask_embedded_scripts("<body><script>b</script></body>"), sp(14) + "b");
    EXPECT_EQ(hot::mask_embedded_scripts("</ HEAD ><script>b</script>", "script", &found), "");
    EXPECT_FALSE(found);
    // A head end tag in a comment or a block does not stop the scan.
    EXPECT_EQ(hot::mask_embedded_scripts("<!-- </head> --><script>a</head>b</script>"), sp(16 + 8) + "a</head>b");
}

TEST(EmbeddedScript, CustomTag)
{
    const std::string text = "<head>\n<script>int s;</script>\n<code>int c;</code>\n</head>";
    EXPECT_EQ(hot::mask_embedded_scripts(text, "script"), "\n" + sp(8) + "int s;");
    EXPECT_EQ(hot::mask_embedded_scripts(text, "code"), "\n\n" + sp(6) + "int c;");
    bool found = true;
    EXPECT_EQ(hot::mask_embedded_scripts("<script>a</script>", "code", &found), "");
    EXPECT_FALSE(found);
}

TEST(EmbeddedScript, LineCountProperty)
{
    // Each text ends with the end tag of the last block. The output has the same number of lines.
    const std::vector<std::string> texts = {
        "<script>a</script>",
        "<head>\n<script>\nint a;\n</script>\n<p>x</p>\n<script>\nint b;\n</script>",
        "<!--\n<script>\nx\n</script>\n-->\r\n<div\n title=\"a\nb\">\n<script>\r\na\r\n</script>",
        "<p>\n\n\n<script src=\"x\">\n</script>\n\n<script>\n\n</script\n>\n<script>q</script>",
        "<script>\n</script>\n<code>\n</code>\n<script>\n  b\n</script >",
        "\n\n<script>a</script>\n<script>\nb\n</script>",
    };
    for (const std::string& text : texts)
    {
        const std::string out = hot::mask_embedded_scripts(text);
        EXPECT_EQ(lines(out), lines(text)) << text;
        EXPECT_EQ(lines(hot::mask_embedded_scripts(text, "script", nullptr, "P;")), lines(text)) << text;
    }
}

std::string stubs(const std::string& text, const std::string& masked = "")
{
    return hot::append_handler_stubs(text, masked, "P;", "void {name}() { {expr}; }");
}

std::string stub_line(const std::string& expr)
{
    return "\nvoid " + hot::handler_stub_name(expr) + "() { " + expr + "; }";
}

TEST(HandlerStubs, NameIsFnv1aHex)
{
    EXPECT_EQ(hot::handler_stub_name(""), "rml_on_cbf29ce484222325");
    EXPECT_EQ(hot::handler_stub_name("a"), "rml_on_af63dc4c8601ec8c");
}

TEST(HandlerStubs, NoHandler)
{
    bool any = true;
    EXPECT_EQ(hot::append_handler_stubs("<p class=\"a\">x</p>", "M", "P;", "{expr}", &any), "M");
    EXPECT_FALSE(any);
}

TEST(HandlerStubs, PrefixPartIsSkipped)
{
    EXPECT_EQ(stubs("<a onclick=\"sound:x; f(); event:y\"/>", "M"), "M" + stub_line("f()"));
    EXPECT_EQ(stubs("<a onclick=\"a::b()\"/>", "M"), "M" + stub_line("a::b()"));
}

TEST(HandlerStubs, SplitKeepsSemicolonInString)
{
    EXPECT_EQ(stubs("<a onclick=\"f('a;b'); g('\\';')\"/>", "M"), "M" + stub_line("f('a;b')") + stub_line("g('\\';')"));
}

TEST(HandlerStubs, EntitiesAreDecoded)
{
    EXPECT_EQ(stubs("<a ONCLICK=\"x &lt; 1 &amp;&amp; y &gt; &quot;z&quot;\"/>", "M"), "M" + stub_line("x < 1 && y > \"z\""));
}

TEST(HandlerStubs, SingleQuotedAttribute)
{
    EXPECT_EQ(stubs("<a onclick='f(\"x\")'/>", "M"), "M" + stub_line("f(\"x\")"));
}

TEST(HandlerStubs, DuplicateGivesOneStub)
{
    EXPECT_EQ(stubs("<a onclick=\"f()\"/><b onmouseover=\"g(); f()\"/>", "M"), "M" + stub_line("f()") + stub_line("g()"));
}

TEST(HandlerStubs, HandlerOnlyDocumentStartsWithPrefix)
{
    bool any = false;
    EXPECT_EQ(hot::append_handler_stubs("<a onclick=\"f()\"/>", "", "P;", "{name}={expr}", &any),
        "P;\n" + hot::handler_stub_name("f()") + "=f()");
    EXPECT_TRUE(any);
}

TEST(HandlerStubs, CommentAndScriptBodyAreIgnored)
{
    const std::string text = "<!-- <a onclick=\"c()\"/> --><script>x = \"<a onclick='s()'>\";</script>"
                             "<style>a { } </style><a onclick=\"r()\"/>";
    EXPECT_EQ(stubs(text, "M"), "M" + stub_line("r()"));
}

TEST(HandlerStubs, StrayQuoteInputEnds)
{
    auto done = std::make_shared<std::promise<std::string>>();
    auto fut = done->get_future();
    std::thread([done] { done->set_value(hot::append_handler_stubs("<a \"x>y\">", "M", "P;", "{expr}")); }).detach();
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(fut.get(), "M");
}

TEST(HandlerStubs, NameOnIsNoHandler)
{
    EXPECT_EQ(stubs("<a on=\"f()\"/>", "M"), "M");
}

TEST(HandlerStubs, TemplateKeepsLineBreaksAndComment)
{
    const std::string out = hot::append_handler_stubs("<a onclick=\"f() // x\"/>", "M", "P;", "void {name}() {\n{expr}\n;}");
    EXPECT_EQ(out, "M\nvoid " + hot::handler_stub_name("f() // x") + "() {\nf() // x\n;}");
}

TEST(HandlerStubs, PartEndingWithColonIsSkipped)
{
    EXPECT_EQ(stubs("<a onclick=\"sound:; event: ; f()\"/>", "M"), "M" + stub_line("f()"));
}

TEST(HandlerStubs, UnquotedValue)
{
    EXPECT_EQ(stubs("<a onclick=f() id=1/>", "M"), "M" + stub_line("f()"));
}

TEST(HandlerStubs, GreaterThanInQuotedValue)
{
    EXPECT_EQ(stubs("<a onclick=\"if (a > b) f()\"/>", "M"), "M" + stub_line("if (a > b) f()"));
}

TEST(HandlerStubs, CdataAndProcessingInstructionAreSkipped)
{
    EXPECT_EQ(stubs("<![CDATA[ <a onclick=\"c()\"> ]]><?pi <a onclick=\"p()\"> ?><a onclick=\"r()\"/>", "M"), "M" + stub_line("r()"));
}

TEST(HandlerStubs, DataEventAttributeIsNoHandler)
{
    EXPECT_EQ(stubs("<a data-event-click=\"f()\"/>", "M"), "M");
}

TEST(HandlerStubs, PlaceholderTextInExpressionStaysUnchanged)
{
    EXPECT_EQ(hot::append_handler_stubs("<a onclick=\"f('{name}{expr}')\"/>", "M", "P;", "{expr}"), "M\nf('{name}{expr}')");
}

} // namespace
