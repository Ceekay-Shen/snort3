//--------------------------------------------------------------------------
// Copyright (C) 2016-2016 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------
// file_log.cc author Hui Cao <huica@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <time.h>

#include <iostream>
#include <string>

#include "file_config.h"
#include "file_flows.h"
#include "file_lib.h"

#include "main/snort_debug.h"
#include "main/snort_types.h"
#include "framework/data_bus.h"
#include "framework/inspector.h"
#include "framework/module.h"
#include "log/messages.h"
#include "log/text_log.h"
#include "protocols/packet.h"
#include "profiler/profiler.h"
#include "utils/stats.h"
#include "utils/util.h"
#include "flow/flow.h"
#include "sfip/sfip_t.h"
#include "time/packet_time.h"

static const char* s_name = "file_log";
static const char* f_name = "file.log";
static const char* s_help = "log file event to file.log";

struct FileLogConfig
{
    bool log_sys_time = false;
    bool log_pkt_time = true;
};

struct FileLogStats
{
    PegCount total_events;
};

static THREAD_LOCAL FileLogStats fl_stats;

static const PegInfo fl_pegs[] =
{
    { "total events", "total file events" },

    { nullptr, nullptr }
};

//-------------------------------------------------------------------------
// log stuff
//-------------------------------------------------------------------------

static THREAD_LOCAL TextLog* tlog = nullptr;

static void dl_tinit()
{
    tlog = TextLog_Init(f_name, 64*K_BYTES, 1*M_BYTES);
}

static void dl_tterm()
{
    TextLog_Term(tlog);
}

//-------------------------------------------------------------------------
// data stuff
//-------------------------------------------------------------------------

class LogHandler : public DataHandler
{
public:
    LogHandler(FileLogConfig& conf)
    { config = conf; }

    void handle(DataEvent& e, Flow*);

private:
    FileLogConfig config;
    void log_file_name(TextLog* log, FileContext* file);
};

void LogHandler::log_file_name(TextLog* log, FileContext* file)
{
    std::string& name = file->get_file_name();

    if (name.length() <= 0)
        return;

    size_t start = name.rfind('/');

    // Move beyond /
    if (start)
        start++;

    TextLog_Puts(log, "[Name: ");
    TextLog_Putc(log, '"');

    for (size_t i = start; i < name.length(); i++)
    {
        if ( name[i] > 0x1F && name[i] < 0x7F)
        {   /* printable */
            TextLog_Putc(log, name[i]);
        }
        else
        {   /* not printable */
            TextLog_Putc(log, '.');
        }
    }

    TextLog_Puts(log, "\"] ");
}

void LogHandler::handle(DataEvent& , Flow* f)
{
    if (config.log_sys_time)
    {
        struct timeval sys_time;
        gettimeofday(&sys_time, nullptr);
        char timestamp[TIMEBUF_SIZE];
        ts_print(&sys_time, timestamp);
        TextLog_Puts(tlog, timestamp);
        TextLog_Print(tlog, " ");
    }

    if (config.log_pkt_time)
    {
        struct timeval pkt_time;
        packet_gettimeofday(&pkt_time);
        char timestamp[TIMEBUF_SIZE];
        ts_print(&pkt_time, timestamp);
        TextLog_Puts(tlog, timestamp);
        TextLog_Print(tlog, " ");
    }

    TextLog_Print(tlog, " %s:%d -> ", sfip_to_str(&f->client_ip), f->client_port);
    TextLog_Print(tlog, "%s:%d, ", sfip_to_str(&f->server_ip), f->server_port);

    FileFlows* files = FileFlows::get_file_flows(f);

    if (!files)
        return;

    FileContext* file = files->get_current_file_context();

    if (!file)
        return;

    log_file_name(tlog, file);

    TextLog_Print(tlog, "[Verdict: %s] ", VerdictName[file->verdict].c_str());

    std::string type_name =
        file_type_name(file->get_file_type()).c_str();

    TextLog_Print(tlog, "[Type: %s] ", type_name.c_str());

    uint8_t* sha = file->get_file_sig_sha256();
    if (sha)
        TextLog_Print(tlog, "[SHA: %s] ", (file->sha_to_string(sha)).c_str());

    uint64_t fsize = file->get_file_size();
    if ( fsize > 0)
        TextLog_Print(tlog, "[Size: %u] ", fsize);

    TextLog_Print(tlog, "\n");

    fl_stats.total_events++;
}

//-------------------------------------------------------------------------
// inspector stuff
//-------------------------------------------------------------------------

class FileLog : public Inspector
{
public:
    FileLog(FileLogConfig& conf) { config = conf; }

    void show(SnortConfig*) override;
    void eval(Packet*) override { };

    bool configure(SnortConfig*) override
    {
        get_data_bus().subscribe("file_event", new LogHandler(config));
        return true;
    }

private:
    FileLogConfig config;
};

void FileLog::show(SnortConfig*)
{
    LogMessage("%s config:\n", s_name);
    LogMessage("    Log system time: %s\n", config.log_sys_time? "true" : "false");
    LogMessage("    Log packet time: %s\n", config.log_pkt_time? "true" : "false");
}

//-------------------------------------------------------------------------
// module stuff
//-------------------------------------------------------------------------

static const Parameter fl_params[] =
{
    { "log_pkt_time", Parameter::PT_BOOL, nullptr, "true",
      "log the packet time when event generated" },

    { "log_sys_time", Parameter::PT_BOOL, nullptr, "false",
      "log the system time when event generated" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

class FileLogModule : public Module
{
public:
    FileLogModule() : Module(s_name, s_help, fl_params)
    { }

    const PegInfo* get_pegs() const override
    { return fl_pegs; }

    PegCount* get_counts() const override
    { return (PegCount*)&fl_stats; }

    bool set(const char*, Value& v, SnortConfig*) override;

public:
    FileLogConfig config;
};

bool FileLogModule::set(const char*, Value& v, SnortConfig*)
{
    if ( v.is("log_pkt_time") )
        config.log_pkt_time = v.get_bool();

    else if ( v.is("log_sys_time") )
        config.log_sys_time = v.get_bool();

    else
        return false;

    return true;
}

//-------------------------------------------------------------------------
// api stuff
//-------------------------------------------------------------------------

static Module* mod_ctor()
{ return new FileLogModule; }

static void mod_dtor(Module* m)
{ delete m; }

static Inspector* fl_ctor(Module* m)
{
    FileLogModule* fl_module = (FileLogModule *)m;
    return new FileLog(fl_module->config);
}

static void fl_dtor(Inspector* p)
{
    delete p;
}

static const InspectApi fl_api
{
    {
        PT_INSPECTOR,
        sizeof(InspectApi),
        INSAPI_VERSION,
        0,
        API_RESERVED,
        API_OPTIONS,
        s_name,
        s_help,
        mod_ctor,
        mod_dtor
    },
    IT_PASSIVE,
    (uint16_t)PktType::NONE,
    nullptr, // buffers
    nullptr, // service
    nullptr, // pinit
    nullptr, // pterm
    dl_tinit,
    dl_tterm,
    fl_ctor,
    fl_dtor,
    nullptr, // ssn
    nullptr  // reset
};

const BaseApi* file_inspectors[] =
{
    &fl_api.base,
    nullptr
};

