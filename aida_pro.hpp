#pragma once

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <regex>
#include <future>
#include <sstream>
#include <map>
#include <set>
#include <fstream>
#include <mutex>
#include <condition_variable>

#include <allow_deprecated.hpp>
#include <auto.hpp>
#include <bitrange.hpp>
#include <bytes.hpp>
#include <compress.hpp>
#include <config.hpp>
#include <cvt64.hpp>
#include <dbg.hpp>
#include <demangle.hpp>
#include <diff3.hpp>
#include <dirtree.hpp>
#include <diskio.hpp>
#include <entry.hpp>
#include <err.h>
#include <exehdr.h>
#include <expr.hpp>
#include <fixup.hpp>
#include <fpro.h>
#include <frame.hpp>
#include <funcs.hpp>
#include <gdl.hpp>
#include <graph.hpp>
#include <help.h>
#include <hexrays.hpp>
#include <ida.hpp>
#include <idacfg.hpp>
#include <idalib.hpp>
#include <ida_highlighter.hpp>
#include <idd.hpp>
#include <idp.hpp>
#include <ieee.h>
#include <intel.hpp>
#include <jumptable.hpp>
#include <kernwin.hpp>
#include <lex.hpp>
#include <libfuncs.hpp>
#include <lines.hpp>
#include <llong.hpp>
#include <loader.hpp>
#include <make_script_ns.hpp>
#include <md5.h>
#include <merge.hpp>
#include <mergemod.hpp>
#include <moves.hpp>
#include <nalt.hpp>
#include <name.hpp>
#include <netnode.hpp>
#include <network.hpp>
#include <offset.hpp>
#include <parsejson.hpp>
#include <pro.h>
#include <problems.hpp>
#include <prodir.h>
#include <pronet.h>
#include <range.hpp>
#include <regex.h>
#include <regfinder.hpp>
#include <registry.hpp>
#include <search.hpp>
#include <segment.hpp>
#include <segregs.hpp>
#include <srclang.hpp>
#include <strlist.hpp>
#include <tryblks.hpp>
#include <typeinf.hpp>
#include <ua.hpp>
#include <undo.hpp>
#include <workarounds.hpp>
#include <xref.hpp>

// Not repeating myself
static inline bool is_word_char(char c)
{
    return qisalnum(c) || c == '_' || c == ':';
}

#include "settings.hpp"
#include "prompts.hpp"
#include "ai_client.hpp"
#include "ida_utils.hpp"
#include "ui.hpp"
#include "actions.hpp"
#include "aida.hpp"