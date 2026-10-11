#pragma once
// The server's command tree (ClientboundCommandsPacket) and a client-side Brigadier subset over it, for the chat
// box like CommandSuggestions: CommandDispatcher.parse (literals exactly, the brigadier argument types exactly, the
// game's argument types by their token shape), the suggestion context at the cursor (local literal / boolean
// suggestions; other arguments ask the server), getSmartUsage and the argument colouring of formatText.
#include "net/protocol.h"
#include "ui/text.h"

#include <memory>
#include <string>
#include <vector>

namespace ui::cmd {

struct Node {
    u8 type = 0;  // 0 root, 1 literal, 2 argument
    bool executable = false;
    int redirect = -1;
    std::vector<int> children;
    std::string name;    // literal text / argument name
    std::string parser;  // argument type ("brigadier:integer", "minecraft:entity")
    u8 numFlags = 0;     // brigadier numbers: 1 min, 2 max
    double min = 0, max = 0;
    int stringKind = 0;  // brigadier:string: 0 word, 1 quotable phrase, 2 greedy phrase
    std::string suggestions;  // custom suggestion provider ("minecraft:ask_server"), "" none
};

struct Tree {
    std::vector<Node> nodes;
    int root = -1;
    bool valid() const { return root >= 0 && root < int(nodes.size()); }
};

// argTypes: the command_argument_type registry by network id. false: malformed / unknown argument type.
bool decode(net::Reader& r, const std::vector<std::string>& argTypes, Tree& out);

struct Range {
    int start = 0, end = 0;
};

// CommandSyntaxException: a translation key with arguments, and the input / cursor of createWithContext.
struct Error {
    std::string key;
    std::vector<std::string> args;
    bool literalIncorrect = false;
    bool hasContext = false;
    std::string input;
    int cursor = 0;
};

// CommandContextBuilder: parsed nodes and arguments with their ranges; child: the context after a redirect.
struct Context {
    int rootNode = -1;
    std::vector<std::pair<int, Range>> nodes;
    std::vector<std::pair<std::string, Range>> args;
    Range range{0, 0};
    bool hasRange = false;
    std::shared_ptr<Context> child;
    const Context& lastChild() const { return child ? child->lastChild() : *this; }
};

struct Parse {
    std::shared_ptr<Context> ctx;
    std::string input;
    int cursor = 0;  // the reader after parsing
    std::vector<std::pair<int, Error>> exceptions;
    bool canRead() const { return cursor < int(input.size()); }
};

// CommandDispatcher.parse from `start` (after the slash).
Parse parse(const Tree& t, const std::string& input, int start);

// CommandDispatcher.getCompletionSuggestions: the context at the cursor. Local suggestions when every candidate
// child is a literal or a boolean; askServer otherwise (the server completes it).
struct Completion {
    int parent = -1;    // SuggestionContext.parent
    int startPos = 0;   // SuggestionContext.startPos
    int start = 0;      // suggestion range start (min(startPos, cursor))
    bool askServer = false;
    std::vector<std::string> local;  // sorted like Suggestions.create (case-insensitive)
};
Completion complete(const Tree& t, const Parse& p, int cursor);

// CommandDispatcher.getSmartUsage(parent): the usage of each non-literal child.
std::vector<std::string> argumentUsage(const Tree& t, int parent);

// The exception's chat text (CommandSuggestions.getExceptionMessage).
Text errorText(const Error& e, const Lang* lang);
// Commands.getParseException: null when the reader is at the end.
bool parseException(const Parse& p, Error& out);
Error unknownArgument(const Parse& p);  // dispatcherUnknownArgument().createWithContext(reader)

// CommandSuggestions.formatText: literals gray, arguments in rotating colours, the unparsed rest red.
Text format(const Parse& p, const std::string& text, int offset);

}  // namespace ui::cmd
