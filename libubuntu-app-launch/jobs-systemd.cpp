/*
 * Copyright © 2016 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *     Ted Gould <ted.gould@canonical.com>
 */

#include "jobs-systemd.h"
#include "application-impl-base.h"
#include "registry-impl.h"
#include "second-exec-core.h"
#include "string-util.h"
#include "utils.h"

extern "C" {
#include "ubuntu-app-launch-trace.h"
}

#include <gio/gio.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <numeric>
#include <regex>
#include <unity/util/GlibMemory.h>

using namespace unity::util;

namespace ubuntu
{
namespace app_launch
{
namespace jobs
{
namespace instance
{

class SystemD : public instance::Base
{
    friend class manager::SystemD;

public:
    explicit SystemD(const AppID& appId,
                     const std::string& job,
                     const std::string& instance,
                     const std::vector<Application::URL>& urls,
                     const std::shared_ptr<Registry::Impl>& registry);
    virtual ~SystemD()
    {
        g_debug("Destroying a SystemD for '%s' instance '%s'", std::string(appId_).c_str(), instance_.c_str());
    }

    /* Query lifecycle */
    pid_t primaryPid() override;
    std::vector<pid_t> pids() override;

    /* Manage lifecycle */
    void stop() override;

};  // class SystemD

SystemD::SystemD(const AppID& appId,
                 const std::string& job,
                 const std::string& instance,
                 const std::vector<Application::URL>& urls,
                 const std::shared_ptr<Registry::Impl>& registry)
    : Base(appId, job, instance, urls, registry)
{
    g_debug("Creating a new SystemD for '%s' instance '%s'", std::string(appId).c_str(), instance.c_str());
}

pid_t SystemD::primaryPid()
{
    auto manager = std::dynamic_pointer_cast<manager::SystemD>(registry_->jobs());
    return manager->unitPrimaryPid(appId_, job_, instance_);
}

std::vector<pid_t> SystemD::pids()
{
    auto manager = std::dynamic_pointer_cast<manager::SystemD>(registry_->jobs());
    return manager->unitPids(appId_, job_, instance_);
}

void SystemD::stop()
{
    auto manager = std::dynamic_pointer_cast<manager::SystemD>(registry_->jobs());
    manager->stopUnit(appId_, job_, instance_);
}

}  // namespace instance

namespace manager
{

static const char* SYSTEMD_DBUS_ADDRESS{"org.freedesktop.systemd1"};
static const char* SYSTEMD_DBUS_IFACE_MANAGER{"org.freedesktop.systemd1.Manager"};
static const char* SYSTEMD_DBUS_PATH_MANAGER{"/org/freedesktop/systemd1"};
// static const char * SYSTEMD_DBUS_IFACE_UNIT{"org.freedesktop.systemd1.Unit"};
static const char* SYSTEMD_DBUS_IFACE_SERVICE{"org.freedesktop.systemd1.Service"};

SystemD::SystemD(const std::shared_ptr<Registry::Impl>& registry)
    : Base(registry)
    , handle_unitNew(DBusSignalUnsubscriber{})
    , handle_unitRemoved(DBusSignalUnsubscriber{})
    , handle_appFailed(DBusSignalUnsubscriber{})
{
    auto gcgroup_root = getenv("UBUNTU_APP_LAUNCH_SYSTEMD_CGROUP_ROOT");
    if (gcgroup_root == nullptr)
    {
        auto cpath = g_build_filename("/sys", "fs", "cgroup", "systemd", nullptr);
        cgroup_root_ = cpath;
        g_free(cpath);
    }
    else
    {
        cgroup_root_ = gcgroup_root;
    }

    if (getenv("UBUNTU_APP_LAUNCH_SYSTEMD_NO_RESET") != nullptr)
    {
        noResetUnits_ = true;
    }

    setupUserbus(registry);
}

void SystemD::setupUserbus(const std::shared_ptr<Registry::Impl>& reg)
{
    auto cancel = reg->thread.getCancellable();
    userbus_ = reg->thread.executeOnThread<std::shared_ptr<GDBusConnection>>([this, cancel]() {
        GError* error = nullptr;
        auto bus = std::shared_ptr<GDBusConnection>(
            [&]() -> GDBusConnection* {
                if (g_file_test(SystemD::userBusPath().c_str(), G_FILE_TEST_EXISTS))
                {
                    return g_dbus_connection_new_for_address_sync(
                        ("unix:path=" + userBusPath()).c_str(), /* path to the user bus */
                        (GDBusConnectionFlags)(
                            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), /* It is a message bus */
                        nullptr,                                             /* observer */
                        cancel.get(),                                        /* cancellable from the thread */
                        &error);                                             /* error */
                }
                else
                {
                    /* Fallback mostly for testing */
                    g_debug("Using session bus for systemd user bus");
                    return g_bus_get_sync(G_BUS_TYPE_SESSION, /* type */
                                          cancel.get(),       /* thread cancellable */
                                          &error);            /* error */
                }
            }(),
            [](GDBusConnection* bus) { g_clear_object(&bus); });

        if (error != nullptr)
        {
            std::string message = std::string("Unable to connect to user bus: ") + error->message;
            g_error_free(error);
            throw std::runtime_error(message);
        }

        /* If we don't subscribe, it doesn't send us signals */
        g_dbus_connection_call(bus.get(),                  /* user bus */
                               SYSTEMD_DBUS_ADDRESS,       /* bus name */
                               SYSTEMD_DBUS_PATH_MANAGER,  /* path */
                               SYSTEMD_DBUS_IFACE_MANAGER, /* interface */
                               "Subscribe",                /* method */
                               nullptr,                    /* params */
                               nullptr,                    /* ret type */
                               G_DBUS_CALL_FLAGS_NONE,     /* flags */
                               -1,                         /* timeout */
                               cancel.get(),               /* cancellable */
                               [](GObject* obj, GAsyncResult* res, gpointer user_data) {
                                   GError* error{nullptr};
                                   unique_glib(g_dbus_connection_call_finish(G_DBUS_CONNECTION(obj), res, &error));

                                   if (error != nullptr)
                                   {
                                       if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                                       {
                                           g_warning("Unable to subscribe to SystemD: %s", error->message);
                                       }
                                       g_error_free(error);
                                       return;
                                   }

                                   g_debug("Subscribed to Systemd");
                               },
                               nullptr);

        /* Setup Unit add/remove signals */
        handle_unitNew = managedDBusSignalConnection(
            g_dbus_connection_signal_subscribe(bus.get(),                  /* bus */
                                               nullptr,                    /* sender */
                                               SYSTEMD_DBUS_IFACE_MANAGER, /* interface */
                                               "UnitNew",                  /* signal */
                                               SYSTEMD_DBUS_PATH_MANAGER,  /* path */
                                               nullptr,                    /* arg0 */
                                               G_DBUS_SIGNAL_FLAGS_NONE,
                                               [](GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                                  const gchar*, GVariant* params, gpointer user_data) -> void {
                                                   auto pthis = static_cast<SystemD*>(user_data);

                                                   if (!g_variant_check_format_string(params, "(so)", FALSE))
                                                   {
                                                       g_warning("Got 'UnitNew' signal with unknown parameter type: %s",
                                                                 g_variant_get_type_string(params));
                                                       return;
                                                   }

                                                   const gchar* unitname{nullptr};
                                                   const gchar* unitpath{nullptr};

                                                   g_variant_get(params, "(&s&o)", &unitname, &unitpath);

                                                   if (unitname == nullptr || unitpath == nullptr)
                                                   {
                                                       g_warning("Got 'UnitNew' signal with funky params %p, %p",
                                                                 unitname, unitpath);
                                                       return;
                                                   }

                                                   try
                                                   {
                                                       pthis->parseUnit(unitname);
                                                   }
                                                   catch (std::runtime_error& e)
                                                   {
                                                       /* Not for UAL */
                                                       g_debug("Unable to parse unit: %s", unitname);
                                                       return;
                                                   }

                                                   try
                                                   {
                                                       auto info = pthis->unitNew(unitname, unitpath, pthis->userbus_);
                                                       pthis->sig_jobStarted(info.job, info.appid, info.inst);
                                                   }
                                                   catch (std::runtime_error& e)
                                                   {
                                                       g_warning("%s", e.what());
                                                   }
                                               },        /* callback */
                                               this,     /* user data */
                                               nullptr), /* user data destroy */
            bus);

        handle_unitRemoved = managedDBusSignalConnection(
            g_dbus_connection_signal_subscribe(
                bus.get(),                  /* bus */
                nullptr,                    /* sender */
                SYSTEMD_DBUS_IFACE_MANAGER, /* interface */
                "UnitRemoved",              /* signal */
                SYSTEMD_DBUS_PATH_MANAGER,  /* path */
                nullptr,                    /* arg0 */
                G_DBUS_SIGNAL_FLAGS_NONE,
                [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* params,
                   gpointer user_data) -> void {
                    auto pthis = static_cast<SystemD*>(user_data);

                    if (!g_variant_check_format_string(params, "(so)", FALSE))
                    {
                        g_warning("Got 'UnitRemoved' signal with unknown parameter type: %s",
                                  g_variant_get_type_string(params));
                        return;
                    }

                    const gchar* unitname{nullptr};
                    const gchar* unitpath{nullptr};

                    g_variant_get(params, "(&s&o)", &unitname, &unitpath);

                    if (unitname == nullptr || unitpath == nullptr)
                    {
                        g_warning("Got 'UnitRemoved' signal with funky params %p, %p", unitname, unitpath);
                        return;
                    }

                    try
                    {
                        pthis->parseUnit(unitname);
                    }
                    catch (std::runtime_error& e)
                    {
                        /* Not for UAL */
                        g_debug("Unable to parse unit: %s", unitname);
                        return;
                    }

                    pthis->unitRemoved(unitname, unitpath);
                },        /* callback */
                this,     /* user data */
                nullptr), /* user data destroy */
            bus);

        getInitialUnits(bus, cancel);

        return bus;
    });
}

SystemD::~SystemD()
{
}

void SystemD::getInitialUnits(const std::shared_ptr<GDBusConnection>& bus, const std::shared_ptr<GCancellable>& cancel)
{
    GError* error = nullptr;

    auto callt = unique_glib(g_dbus_connection_call_sync(bus.get(),                         /* user bus */
                                                         SYSTEMD_DBUS_ADDRESS,              /* bus name */
                                                         SYSTEMD_DBUS_PATH_MANAGER,         /* path */
                                                         SYSTEMD_DBUS_IFACE_MANAGER,        /* interface */
                                                         "ListUnits",                       /* method */
                                                         nullptr,                           /* params */
                                                         G_VARIANT_TYPE("(a(ssssssouso))"), /* ret type */
                                                         G_DBUS_CALL_FLAGS_NONE,            /* flags */
                                                         -1,                                /* timeout */
                                                         cancel.get(),                      /* cancellable */
                                                         &error));

    if (error != nullptr)
    {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            g_warning("Unable to list SystemD units: %s", error->message);
        }
        g_error_free(error);
        return;
    }

    auto call = unique_glib(g_variant_get_child_value(callt.get(), 0));

    const gchar* id;
    const gchar* description;
    const gchar* loadState;
    const gchar* activeState;
    const gchar* subState;
    const gchar* following;
    const gchar* path;
    guint32 jobId;
    const gchar* jobType;
    const gchar* jobPath;
    auto iter = unique_glib(g_variant_iter_new(call.get()));
    while (g_variant_iter_loop(iter.get(), "(&s&s&s&s&s&s&ou&s&o)", &id, &description, &loadState, &activeState,
                               &subState, &following, &path, &jobId, &jobType, &jobPath))
    {
        try
        {
            unitNew(id, jobPath, bus);
        }
        catch (std::runtime_error& e)
        {
        }
    }
}

std::string SystemD::findEnv(const std::string& value, std::list<std::pair<std::string, std::string>>& env)
{
    std::string retval;
    auto entry = std::find_if(env.begin(), env.end(),
                              [&value](std::pair<std::string, std::string>& entry) { return entry.first == value; });

    if (entry != env.end())
    {
        retval = entry->second;
    }

    return retval;
}

void SystemD::removeEnv(const std::string& value, std::list<std::pair<std::string, std::string>>& env)
{
    auto entry = std::find_if(env.begin(), env.end(),
                              [&value](std::pair<std::string, std::string>& entry) { return entry.first == value; });

    if (entry != env.end())
    {
        env.erase(entry);
    }
}

int SystemD::envSize(std::list<std::pair<std::string, std::string>>& env)
{
    int len = std::string{"Environment="}.length();

    for (const auto& entry : env)
    {
        len += 3; /* two quotes, one space */
        len += entry.first.length();
        len += entry.second.length();
    }

    len -= 1; /* We account for a space each time but the first doesn't have */

    return len;
}

std::vector<std::string> SystemD::parseExec(std::list<std::pair<std::string, std::string>>& env)
{
    auto exec = findEnv("APP_EXEC", env);
    if (exec.empty())
    {
        g_warning("Application exec line is empty?!?!?");
        return {};
    }
    auto uris = findEnv("APP_URIS", env);

    g_debug("Exec line: %s", exec.c_str());
    g_debug("App URLS:  %s", uris.c_str());

    auto execarray = desktop_exec_parse(exec.c_str(), uris.c_str());

    std::vector<std::string> retval;
    for (unsigned int i = 0; i < execarray->len; i++)
    {
        auto cstr = g_array_index(execarray, gchar*, i);
        if (cstr != nullptr)
        {
            retval.emplace_back(cstr);
        }
    }

    /* This seems to work better than array_free(), I can't figure out why */
    auto strv = (gchar**)g_array_free(execarray, FALSE);
    g_strfreev(strv);

    if (retval.empty())
    {
        g_warning("After parsing 'APP_EXEC=%s' we ended up with no tokens", exec.c_str());
    }

    /* See if we're doing apparmor by hand */
    auto appexecpolicy = findEnv("APP_EXEC_POLICY", env);
    if (!appexecpolicy.empty() && appexecpolicy != "unconfined")
    {
        retval.emplace(retval.begin(), appexecpolicy);
        retval.emplace(retval.begin(), "-p");
        retval.emplace(retval.begin(), "aa-exec");
    }

    return retval;
}

/** Small helper that we can new/delete to work better with C stuff */
struct StartCHelper
{
    std::shared_ptr<instance::SystemD> ptr;
    std::shared_ptr<GDBusConnection> bus;
};

void SystemD::application_start_cb(GObject* obj, GAsyncResult* res, gpointer user_data)
{
    auto data = static_cast<StartCHelper*>(user_data);

    tracepoint(ubuntu_app_launch, libual_start_message_callback, std::string(data->ptr->appId_).c_str());

    g_debug("Started Message Callback: %s", std::string(data->ptr->appId_).c_str());

    GError* error{nullptr};

    /* We don't care about the result but we need to make sure we don't
       have a leak. */
    unique_glib(g_dbus_connection_call_finish(G_DBUS_CONNECTION(obj), res, &error));

    if (error != nullptr)
    {
        if (g_dbus_error_is_remote_error(error))
        {
            gchar* remote_error = g_dbus_error_get_remote_error(error);
            g_debug("Remote error: %s", remote_error);
            if (g_strcmp0(remote_error, "org.freedesktop.systemd1.UnitExists") == 0)
            {
                auto urls = instance::SystemD::urlsToStrv(data->ptr->urls_);
                second_exec(data->bus.get(),                                     /* DBus */
                            data->ptr->registry_->thread.getCancellable().get(), /* cancellable */
                            data->ptr->primaryPid(),                             /* primary pid */
                            std::string(data->ptr->appId_).c_str(),              /* appid */
                            data->ptr->instance_.c_str(),                        /* instance */
                            urls.get());                                         /* urls */
            }

            g_free(remote_error);
        }
        else
        {
            if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            {
                g_warning("Unable to emit event to start application: %s", error->message);
            }
        }
        g_error_free(error);
    }

    delete data;
}

void SystemD::copyEnv(const std::string& envname, std::list<std::pair<std::string, std::string>>& env)
{
    if (!findEnv(envname, env).empty())
    {
        g_debug("Already a value set for '%s' ignoring", envname.c_str());
        return;
    }

    auto cvalue = getenv(envname.c_str());
    g_debug("Copying Environment: %s", envname.c_str());
    if (cvalue != nullptr)
    {
        std::string value{cvalue};
        env.emplace_back(std::make_pair(envname, value));
    }
    else
    {
        g_debug("Unable to copy environment '%s'", envname.c_str());
    }
}

void SystemD::copyEnvByPrefix(const std::string& prefix, std::list<std::pair<std::string, std::string>>& env)
{
    for (unsigned int i = 0; environ[i] != nullptr; i++)
    {
        if (g_str_has_prefix(environ[i], prefix.c_str()))
        {
            std::string envname = environ[i];
            envname.erase(envname.find('='));
            copyEnv(envname, env);
        }
    }
}

std::shared_ptr<Application::Instance> SystemD::launch(
    const AppID& appId,
    const std::string& job,
    const std::string& instance,
    const std::vector<Application::URL>& urls,
    launchMode mode,
    std::function<std::list<std::pair<std::string, std::string>>(void)>& getenv)
{
    if (appId.empty())
        return {};

    auto appJobs = getAllApplicationJobs();
    bool isApplication = std::find(appJobs.begin(), appJobs.end(), job) != appJobs.end();

    auto reg = getReg();
    return reg->thread.executeOnThread<std::shared_ptr<instance::SystemD>>([&]() -> std::shared_ptr<instance::SystemD> {
        auto manager = std::dynamic_pointer_cast<manager::SystemD>(reg->jobs());
        std::string appIdStr{appId};
        g_debug("Initializing params for an new instance::SystemD for: %s", appIdStr.c_str());

        tracepoint(ubuntu_app_launch, libual_start, appIdStr.c_str());

        int timeout = 1;
        if (ubuntu::app_launch::Registry::Impl::isWatchingAppStarting())
        {
            timeout = 0;
        }

        handshake_t* handshake{nullptr};

        if (isApplication)
        {
            handshake = starting_handshake_start(appIdStr.c_str(), instance.c_str(), timeout);
            if (handshake == nullptr)
            {
                g_warning("Unable to setup starting handshake");
            }
        }

        /* Figure out the unit name for the job */
        auto unitname = unitName(SystemD::UnitInfo{appIdStr, job, instance});

        /* Build up our environment */
        auto env = getenv();

        env.emplace_back(std::make_pair("APP_ID", appIdStr));                           /* Application ID */
        env.emplace_back(std::make_pair("APP_LAUNCHER_PID", std::to_string(getpid()))); /* Who we are, for bugs */

        copyEnv("DISPLAY", env);

        for (const auto& prefix : {"DBUS_", "MIR_", "UBUNTU_APP_LAUNCH_"})
        {
            copyEnvByPrefix(prefix, env);
        }

        /* If we're in deb mode and launching legacy apps, they're gonna need
         * more context, they really have no other way to get it. */
        if (g_getenv("SNAP") == nullptr && appId.package.value().empty())
        {
            copyEnvByPrefix("QT_", env);
            copyEnvByPrefix("XDG_", env);

            /* If we're in Unity8 we don't want to pass it's platform, we want
             * an application platform. */
            if (findEnv("QT_QPA_PLATFORM", env) == "mirserver" || findEnv("QT_QPA_PLATFORM", env) == "ubuntumirclient")
            {
                removeEnv("QT_QPA_PLATFORM", env);
                env.emplace_back(std::make_pair("QT_QPA_PLATFORM", "wayland"));
            }
        }

        /* Mir socket if we don't have one in our env */
        if (findEnv("MIR_SOCKET", env).empty())
        {
            env.emplace_back(std::make_pair("MIR_SOCKET", g_get_user_runtime_dir() + std::string{"/mir_socket"}));
        }

        if (!urls.empty())
        {
            auto accumfunc = [](const std::string& prev, Application::URL thisurl) -> std::string {
                gchar* gescaped = g_shell_quote(thisurl.value().c_str());
                std::string escaped;
                if (gescaped != nullptr)
                {
                    escaped = gescaped;
                    g_free(gescaped);
                }
                else
                {
                    g_warning("Unable to escape URL: %s", thisurl.value().c_str());
                    return prev;
                }

                if (prev.empty())
                {
                    return escaped;
                }
                else
                {
                    return prev + " " + escaped;
                }
            };
            auto urlstring = std::accumulate(urls.begin(), urls.end(), std::string{}, accumfunc);
            env.emplace_back(std::make_pair("APP_URIS", urlstring));
        }

        if (mode == launchMode::TEST)
        {
            env.emplace_back(std::make_pair("QT_LOAD_TESTABILITY", "1"));
        }

        /* Convert to GVariant */
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_TUPLE);

        g_variant_builder_add_value(&builder, g_variant_new_string(unitname.c_str()));
        g_variant_builder_add_value(&builder, g_variant_new_string("replace"));  // Job mode

        /* Parameter Array */
        g_variant_builder_open(&builder, G_VARIANT_TYPE_ARRAY);

        /* ExecStart */
        auto commands = parseExec(env);
        if (!commands.empty())
        {
            g_variant_builder_open(&builder, G_VARIANT_TYPE_TUPLE);
            g_variant_builder_add_value(&builder, g_variant_new_string("ExecStart"));
            g_variant_builder_open(&builder, G_VARIANT_TYPE_VARIANT);
            g_variant_builder_open(&builder, G_VARIANT_TYPE_ARRAY);

            g_variant_builder_open(&builder, G_VARIANT_TYPE_TUPLE);

            gchar* pathexec = g_find_program_in_path(commands[0].c_str());
            if (pathexec != nullptr)
            {
                g_variant_builder_add_value(&builder, g_variant_new_take_string(pathexec));
            }
            else
            {
                g_debug("Unable to find '%s' in PATH=%s", commands[0].c_str(), g_getenv("PATH"));
                g_variant_builder_add_value(&builder, g_variant_new_string(commands[0].c_str()));
            }

            g_variant_builder_open(&builder, G_VARIANT_TYPE_ARRAY);
            for (const auto& param : commands)
            {
                g_variant_builder_add_value(&builder, g_variant_new_string(param.c_str()));
            }
            g_variant_builder_close(&builder);

            g_variant_builder_add_value(&builder, g_variant_new_boolean(FALSE));

            g_variant_builder_close(&builder);
            g_variant_builder_close(&builder);
            g_variant_builder_close(&builder);
            g_variant_builder_close(&builder);
        }

        /* RemainAfterExit */
        g_variant_builder_open(&builder, G_VARIANT_TYPE_TUPLE);
        g_variant_builder_add_value(&builder, g_variant_new_string("RemainAfterExit"));
        g_variant_builder_open(&builder, G_VARIANT_TYPE_VARIANT);
        g_variant_builder_add_value(&builder, g_variant_new_boolean(FALSE));
        g_variant_builder_close(&builder);
        g_variant_builder_close(&builder);

        /* Type */
        g_variant_builder_open(&builder, G_VARIANT_TYPE_TUPLE);
        g_variant_builder_add_value(&builder, g_variant_new_string("Type"));
        g_variant_builder_open(&builder, G_VARIANT_TYPE_VARIANT);
        g_variant_builder_add_value(&builder, g_variant_new_string("oneshot"));
        g_variant_builder_close(&builder);
        g_variant_builder_close(&builder);

        /* Working Directory */
        if (!findEnv("APP_DIR", env).empty())
        {
            g_variant_builder_open(&builder, G_VARIANT_TYPE_TUPLE);
            g_variant_builder_add_value(&builder, g_variant_new_string("WorkingDirectory"));
            g_variant_builder_open(&builder, G_VARIANT_TYPE_VARIANT);
            g_variant_builder_add_value(&builder, g_variant_new_string(findEnv("APP_DIR", env).c_str()));
            g_variant_builder_close(&builder);
            g_variant_builder_close(&builder);
        }

        /* Clean up env before shipping it */
        for (const auto& rmenv :
             {"APP_DIR", "APP_URIS", "APP_EXEC", "APP_EXEC_POLICY", "APP_LAUNCHER_PID",
              "INSTANCE_ID", "MIR_SERVER_PLATFORM_PATH", "MIR_SERVER_PROMPT_FILE", "MIR_SERVER_HOST_SOCKET",
              "UBUNTU_APP_LAUNCH_OOM_HELPER", "UBUNTU_APP_LAUNCH_LEGACY_ROOT"})
        {
            removeEnv(rmenv, env);
        }

        g_debug("Environment length: %d", envSize(env));

        /* Environment */
        g_variant_builder_open(&builder, G_VARIANT_TYPE_TUPLE);
        g_variant_builder_add_value(&builder, g_variant_new_string("Environment"));
        g_variant_builder_open(&builder, G_VARIANT_TYPE_VARIANT);
        g_variant_builder_open(&builder, G_VARIANT_TYPE_ARRAY);
        for (const auto& envvar : env)
        {
            if (!envvar.first.empty() && !envvar.second.empty())
            {
                g_variant_builder_add_value(&builder, g_variant_new_take_string(g_strdup_printf(
                                                          "%s=%s", envvar.first.c_str(), envvar.second.c_str())));
                // g_debug("Setting environment: %s=%s", envvar.first.c_str(), envvar.second.c_str());
            }
        }

        g_variant_builder_close(&builder);
        g_variant_builder_close(&builder);
        g_variant_builder_close(&builder);

        /* Parameter Array */
        g_variant_builder_close(&builder);

        /* Dependent Units (none) */
        g_variant_builder_add_value(&builder, g_variant_new_array(G_VARIANT_TYPE("(sa(sv))"), nullptr, 0));

        auto retval = std::make_shared<instance::SystemD>(appId, job, instance, urls, reg);
        auto chelper = new StartCHelper{};
        chelper->ptr = retval;
        chelper->bus = reg->_dbus;

        tracepoint(ubuntu_app_launch, handshake_wait, appIdStr.c_str());
        starting_handshake_wait(handshake);
        tracepoint(ubuntu_app_launch, handshake_complete, appIdStr.c_str());

        /* Call the job start function */
        g_debug("Asking systemd to start task for: %s", appIdStr.c_str());
        g_dbus_connection_call(manager->userbus_.get(),            /* bus */
                               SYSTEMD_DBUS_ADDRESS,               /* service name */
                               SYSTEMD_DBUS_PATH_MANAGER,          /* Path */
                               SYSTEMD_DBUS_IFACE_MANAGER,         /* interface */
                               "StartTransientUnit",               /* method */
                               g_variant_builder_end(&builder),    /* params */
                               G_VARIANT_TYPE("(o)"),              /* return */
                               G_DBUS_CALL_FLAGS_NONE,             /* flags */
                               -1,                                 /* default timeout */
                               reg->thread.getCancellable().get(), /* cancellable */
                               application_start_cb,               /* callback */
                               chelper                             /* object */
                               );

        tracepoint(ubuntu_app_launch, libual_start_message_sent, appIdStr.c_str());

        return retval;
    });
}

std::shared_ptr<Application::Instance> SystemD::existing(const AppID& appId,
                                                         const std::string& job,
                                                         const std::string& instance,
                                                         const std::vector<Application::URL>& urls)
{
    return std::make_shared<instance::SystemD>(appId, job, instance, urls, getReg());
}

std::vector<std::shared_ptr<instance::Base>> SystemD::instances(const AppID& appID, const std::string& job)
{
    auto reg = getReg();

    std::vector<std::shared_ptr<instance::Base>> instances;
    std::vector<Application::URL> urls;

    std::string sappid{appID};
    for (const auto& unit : unitPaths)
    {
        const SystemD::UnitInfo& unitinfo = unit.first;

        if (job != unitinfo.job)
        {
            continue;
        }

        if (sappid != unitinfo.appid)
        {
            continue;
        }

        instances.emplace_back(std::make_shared<instance::SystemD>(appID, job, unitinfo.inst, urls, reg));
    }

    g_debug("Found %d instances for AppID '%s'", int(instances.size()), std::string(appID).c_str());

    return instances;
}

std::list<std::string> SystemD::runningAppIds(const std::list<std::string>& allJobs)
{
    std::set<std::string> appids;

    for (const auto& unit : unitPaths)
    {
        const SystemD::UnitInfo& unitinfo = unit.first;

        if (std::find(allJobs.begin(), allJobs.end(), unitinfo.job) == allJobs.end())
        {
            continue;
        }

        appids.insert(unitinfo.appid);
    }

    return {appids.begin(), appids.end()};
}

std::string SystemD::userBusPath()
{
    auto cpath = getenv("UBUNTU_APP_LAUNCH_SYSTEMD_PATH");
    if (cpath != nullptr)
    {
        return cpath;
    }
    return std::string{"/run/user/"} + std::to_string(getuid()) + std::string{"/bus"};
}

/* TODO: Application job names */
const std::regex unitNaming{"^ubuntu\\-app\\-launch\\-\\-(.*)\\-\\-(.*)\\-\\-([0-9]*)\\.service$"};

SystemD::UnitInfo SystemD::parseUnit(const std::string& unit) const
{
    std::smatch match;
    if (!std::regex_match(unit, match, unitNaming))
    {
        throw std::runtime_error{"Unable to parse unit name: " + unit};
    }

    return {match[2].str(), match[1].str(), match[3].str()};
}

std::string SystemD::unitName(const SystemD::UnitInfo& info) const
{
    return std::string{"ubuntu-app-launch--"} + info.job + "--" + info.appid + "--" + info.inst + ".service";
}

std::string SystemD::unitPath(const SystemD::UnitInfo& info)
{
    auto reg = getReg();
    auto data = unitPaths[info];

    if (!data)
    {
        return {};
    }

    /* Execute on the thread so that we're sure that we're not in
       a dbus call to get the value. No racey for you! */
    return reg->thread.executeOnThread<std::string>([&data]() { return data->unitpath; });
}

SystemD::UnitInfo SystemD::unitNew(const std::string& name,
                                   const std::string& path,
                                   const std::shared_ptr<GDBusConnection>& bus)
{
    if (path == "/")
    {
        throw std::runtime_error{"Job path for unit is '/' so likely failed"};
    }

    auto info = parseUnit(name);

    g_debug("New Unit: %s", name.c_str());

    auto reg = getReg();

    auto data = std::make_shared<UnitData>();
    data->jobpath = path;

    /* We already have this one, continue on */
    if (!unitPaths.insert(std::make_pair(info, data)).second)
    {
        throw std::runtime_error{"Duplicate unit, not really new"};
    }

    /* We need to get the path, we're blocking everyone else on
       this call if they try to get the path. But we're just locking
       up the UAL thread so it should be a big deal. But if someone
       comes an asking at this point we'll think that we have the
       app, but not yet its path */
    GError* error{nullptr};
    auto call = unique_glib(g_dbus_connection_call_sync(bus.get(),                          /* user bus */
                                                        SYSTEMD_DBUS_ADDRESS,               /* bus name */
                                                        SYSTEMD_DBUS_PATH_MANAGER,          /* path */
                                                        SYSTEMD_DBUS_IFACE_MANAGER,         /* interface */
                                                        "GetUnit",                          /* method */
                                                        g_variant_new("(s)", name.c_str()), /* params */
                                                        G_VARIANT_TYPE("(o)"),              /* ret type */
                                                        G_DBUS_CALL_FLAGS_NONE,             /* flags */
                                                        -1,                                 /* timeout */
                                                        reg->thread.getCancellable().get(), /* cancellable */
                                                        &error));

    if (error != nullptr)
    {
        std::string message = "Unable to get SystemD unit path for '" + name + "': " + error->message;
        g_error_free(error);
        throw std::runtime_error{message};
    }

    /* Parse variant */
    gchar* gpath{nullptr};
    g_variant_get(call.get(), "(&o)", &gpath);
    if (gpath)
    {
        data->unitpath = gpath;
    }

    return info;
}

void SystemD::unitRemoved(const std::string& name, const std::string& path)
{
    UnitInfo info = parseUnit(name);

    auto it = unitPaths.find(info);
    if (it != unitPaths.end())
    {
        unitPaths.erase(it);
        sig_jobStopped(info.job, info.appid, info.inst);
    }
}

pid_t SystemD::unitPrimaryPid(const AppID& appId, const std::string& job, const std::string& instance)
{
    auto unitinfo = SystemD::UnitInfo{appId, job, instance};
    auto unitname = unitName(unitinfo);
    auto unitpath = unitPath(unitinfo);

    if (unitpath.empty())
    {
        return 0;
    }

    auto reg = getReg();

    return reg->thread.executeOnThread<pid_t>([this, unitname, unitpath, reg]() {
        GError* error{nullptr};
        auto call = unique_glib(
            g_dbus_connection_call_sync(userbus_.get(),                                               /* user bus */
                                        SYSTEMD_DBUS_ADDRESS,                                         /* bus name */
                                        unitpath.c_str(),                                             /* path */
                                        "org.freedesktop.DBus.Properties",                            /* interface */
                                        "Get",                                                        /* method */
                                        g_variant_new("(ss)", SYSTEMD_DBUS_IFACE_SERVICE, "MainPID"), /* params */
                                        G_VARIANT_TYPE("(v)"),                                        /* ret type */
                                        G_DBUS_CALL_FLAGS_NONE,                                       /* flags */
                                        -1,                                                           /* timeout */
                                        reg->thread.getCancellable().get(),                           /* cancellable */
                                        &error));

        if (error != nullptr)
        {
            auto message =
                std::string{"Unable to get SystemD PID for '"} + unitname + std::string{"': "} + error->message;
            g_error_free(error);
            throw std::runtime_error(message);
        }

        /* Parse variant */
        GVariantUPtr vpid{nullptr, GVariantDeleter{}};
        {
            GVariant* tmp{nullptr};
            g_variant_get(call.get(), "(v)", &tmp);
            vpid = unique_glib(tmp);
        }

        pid_t pid;
        pid = g_variant_get_uint32(vpid.get());

        return pid;
    });
}

std::vector<pid_t> SystemD::unitPids(const AppID& appId, const std::string& job, const std::string& instance)
{
    auto unitinfo = SystemD::UnitInfo{appId, job, instance};
    auto unitname = unitName(unitinfo);
    auto unitpath = unitPath(unitinfo);

    if (unitpath.empty())
    {
        return {};
    }

    auto reg = getReg();

    auto cgrouppath = reg->thread.executeOnThread<std::string>([this, unitname, unitpath, reg]() {
        GError* error{nullptr};
        auto call = unique_glib(
            g_dbus_connection_call_sync(userbus_.get(),                    /* user bus */
                                        SYSTEMD_DBUS_ADDRESS,              /* bus name */
                                        unitpath.c_str(),                  /* path */
                                        "org.freedesktop.DBus.Properties", /* interface */
                                        "Get",                             /* method */
                                        g_variant_new("(ss)", SYSTEMD_DBUS_IFACE_SERVICE, "ControlGroup"), /* params */
                                        G_VARIANT_TYPE("(v)"),              /* ret type */
                                        G_DBUS_CALL_FLAGS_NONE,             /* flags */
                                        -1,                                 /* timeout */
                                        reg->thread.getCancellable().get(), /* cancellable */
                                        &error));

        if (error != nullptr)
        {
            auto message = std::string{"Unable to get SystemD Control Group for '"} + unitname + std::string{"': "} +
                           error->message;
            g_error_free(error);
            throw std::runtime_error(message);
        }

        /* Parse variant */
        GVariantUPtr vstring{nullptr, GVariantDeleter{}};
        {
            GVariant* tmp{nullptr};
            g_variant_get(call.get(), "(v)", &tmp);
            vstring = unique_glib(tmp);
        }

        if (vstring == nullptr)
        {
            return std::string{};
        }

        std::string group;
        auto ggroup = g_variant_get_string(vstring.get(), nullptr);
        if (ggroup != nullptr)
        {
            group = ggroup;
        }

        return group;
    });

    auto fullpath = unique_gchar(g_build_filename(cgroup_root_.c_str(), cgrouppath.c_str(), "tasks", nullptr));
    GError* error = nullptr;

    g_debug("Getting PIDs from %s", fullpath.get());
    GCharUPtr pidstr{nullptr, &g_free};
    {
        gchar* tmp = nullptr;
        g_file_get_contents(fullpath.get(), &tmp, nullptr, &error);
        pidstr = unique_gchar(tmp);
    }

    if (error != nullptr)
    {
        g_warning("Unable to read cgroup PID list: %s", error->message);
        g_error_free(error);
        return {};
    }

    auto pidlines = unique_gcharv(g_strsplit(pidstr.get(), "\n", -1));
    std::vector<pid_t> pids;

    for (auto i = 0; pidlines.get()[i] != nullptr; i++)
    {
        const gchar* pidline = pidlines.get()[i];
        if (pidline[0] != '\n')
        {
            auto pid = std::atoi(pidline);
            if (pid != 0)
            {
                pids.emplace_back(pid);
            }
        }
    }

    return pids;
}

void SystemD::stopUnit(const AppID& appId, const std::string& job, const std::string& instance)
{
    auto unitname = unitName(SystemD::UnitInfo{appId, job, instance});
    auto reg = getReg();

    reg->thread.executeOnThread<bool>([this, unitname, reg] {
        GError* error{nullptr};
        unique_glib(g_dbus_connection_call_sync(
            userbus_.get(),             /* user bus */
            SYSTEMD_DBUS_ADDRESS,       /* bus name */
            SYSTEMD_DBUS_PATH_MANAGER,  /* path */
            SYSTEMD_DBUS_IFACE_MANAGER, /* interface */
            "StopUnit",                 /* method */
            g_variant_new(
                "(ss)",                         /* params */
                unitname.c_str(),               /* param: specify unit */
                "replace-irreversibly"),        /* param: replace the current job but don't allow us to be replaced */
            G_VARIANT_TYPE("(o)"),              /* ret type */
            G_DBUS_CALL_FLAGS_NONE,             /* flags */
            -1,                                 /* timeout */
            reg->thread.getCancellable().get(), /* cancellable */
            &error));

        if (error != nullptr)
        {
            auto message =
                std::string{"Unable to get SystemD to stop '"} + unitname + std::string{"': "} + error->message;
            g_error_free(error);
            throw std::runtime_error(message);
        }

        return true;
    });
}

core::Signal<const std::string&, const std::string&, const std::string&>& SystemD::jobStarted()
{
    /* Ensure we're connecting to the signals */
    return sig_jobStarted;
}

core::Signal<const std::string&, const std::string&, const std::string&>& SystemD::jobStopped()
{
    /* Ensure we're connecting to the signals */
    return sig_jobStopped;
}

struct FailedData
{
    std::weak_ptr<Registry::Impl> registry;
};

core::Signal<const std::string&, const std::string&, const std::string&, Registry::FailureType>& SystemD::jobFailed()
{
    std::call_once(flag_appFailed, [this]() {
        auto reg = getReg();
        reg->thread.executeOnThread<bool>([this, reg]() {
            auto data = new FailedData{reg};

            handle_appFailed = managedDBusSignalConnection(
                g_dbus_connection_signal_subscribe(
                    userbus_.get(),                    /* bus */
                    SYSTEMD_DBUS_ADDRESS,              /* sender */
                    "org.freedesktop.DBus.Properties", /* interface */
                    "PropertiesChanged",               /* signal */
                    nullptr,                           /* path */
                    SYSTEMD_DBUS_IFACE_SERVICE,        /* arg0 */
                    G_DBUS_SIGNAL_FLAGS_NONE,
                    [](GDBusConnection*, const gchar*, const gchar* path, const gchar*, const gchar*, GVariant* params,
                       gpointer user_data) -> void {
                        auto data = static_cast<FailedData*>(user_data);
                        auto reg = data->registry.lock();

                        if (!reg)
                        {
                            throw std::runtime_error{"Lost our connection with the registry"};
                        }

                        auto manager = std::dynamic_pointer_cast<SystemD>(reg->jobs());

                        /* Check to see if this is a path we care about */
                        bool pathfound{false};
                        UnitInfo unitinfo;
                        for (const auto& unit : manager->unitPaths)
                        {
                            if (unit.second->unitpath == path)
                            {
                                pathfound = true;
                                unitinfo = unit.first;
                                break;
                            }
                        }
                        if (!pathfound)
                        {
                            return;
                        }

                        /* Now see if it is a property we care about */
                        auto vdict = unique_glib(g_variant_get_child_value(params, 1));
                        GVariantDict dict;
                        g_variant_dict_init(&dict, vdict.get());

                        if (g_variant_dict_contains(&dict, "Result") == FALSE)
                        {
                            /* We don't care about anything else */
                            g_variant_dict_clear(&dict);
                            return;
                        }

                        /* Check to see if it just was successful */
                        const gchar* value{nullptr};
                        g_variant_dict_lookup(&dict, "Result", "&s", &value);

                        if (g_strcmp0(value, "success") == 0)
                        {
                            g_variant_dict_clear(&dict);
                            return;
                        }
                        g_variant_dict_clear(&dict);

                        /* Reset the failure bit on the unit */
                        manager->resetUnit(unitinfo);

                        /* Oh, we might want to do something now */
                        auto reason{Registry::FailureType::CRASH};
                        if (g_strcmp0(value, "exit-code") == 0)
                        {
                            reason = Registry::FailureType::START_FAILURE;
                        }

                        manager->sig_jobFailed(unitinfo.job, unitinfo.appid, unitinfo.inst, reason);
                    },    /* callback */
                    data, /* user data */
                    [](gpointer user_data) {
                        auto data = static_cast<FailedData*>(user_data);
                        delete data;
                    }), /* user data destroy */
                userbus_);

            return true;
        });
    });

    return sig_jobFailed;
}

/** Requests that systemd reset a unit that has been marked as
    failed so that we can continue to work with it. This includes
    starting it anew, which can fail if it is left in the failed
    state. */
void SystemD::resetUnit(const UnitInfo& info)
{
    if (noResetUnits_)
    {
        return;
    }

    auto reg = getReg();
    auto unitname = unitName(info);
    auto bus = userbus_;
    auto cancel = reg->thread.getCancellable();

    reg->thread.executeOnThread([bus, unitname, cancel] {
        if (g_cancellable_is_cancelled(cancel.get()))
        {
            return;
        }

        g_dbus_connection_call(bus.get(),                       /* user bus */
                               SYSTEMD_DBUS_ADDRESS,            /* bus name */
                               SYSTEMD_DBUS_PATH_MANAGER,       /* path */
                               SYSTEMD_DBUS_IFACE_MANAGER,      /* interface */
                               "ResetFailedUnit",               /* method */
                               g_variant_new("(s)",             /* params */
                                             unitname.c_str()), /* param: specify unit */
                               nullptr,                         /* ret type */
                               G_DBUS_CALL_FLAGS_NONE,          /* flags */
                               -1,                              /* timeout */
                               cancel.get(),                    /* cancellable */
                               [](GObject* obj, GAsyncResult* res, gpointer user_data) {
                                   GError* error{nullptr};
                                   unique_glib(g_dbus_connection_call_finish(G_DBUS_CONNECTION(obj), res, &error));

                                   if (error != nullptr)
                                   {
                                       if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                                       {
                                           g_warning("Unable to reset failed unit: %s", error->message);
                                       }
                                       g_error_free(error);
                                       return;
                                   }

                                   g_debug("Reset Failed Unit");
                               },
                               nullptr);
    });
}

}  // namespace manager
}  // namespace jobs
}  // namespace app_launch
}  // namespace ubuntu
