/*
 * Copyright 2013 Canonical Ltd.
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

#include <gtest/gtest.h>
#include <gio/gio.h>
#include <libdbustest/dbus-test.h>

TEST(ZGEvent, OpenTest)
{
	DbusTestService * service = dbus_test_service_new(NULL);

	DbusTestDbusMock * mock = dbus_test_dbus_mock_new("org.gnome.zeitgeist.Engine");
	DbusTestDbusMockObject * obj = dbus_test_dbus_mock_get_object(mock, "/org/gnome/zeitgeist/log/activity", "org.gnome.zeitgeist.Log", NULL);

	dbus_test_dbus_mock_object_add_method(mock, obj,
		"InsertEvents",
		G_VARIANT_TYPE("a(asaasay)"),
		G_VARIANT_TYPE("au"),
		"ret = [ 0 ]",
		NULL);

	dbus_test_service_add_task(service, DBUS_TEST_TASK(mock));

	DbusTestProcess * zgevent = dbus_test_process_new(ZG_EVENT_TOOL);
	dbus_test_process_append_param(zgevent, "open");
	g_setenv("APP_ID", "foo", 1);
	dbus_test_task_set_wait_for(DBUS_TEST_TASK(zgevent), "org.gnome.zeitgeist.Engine");
	dbus_test_task_set_name(DBUS_TEST_TASK(zgevent), "ZGEvent");

	dbus_test_service_add_task(service, DBUS_TEST_TASK(zgevent));

	dbus_test_service_start_tasks(service);

	/* Give it time to send the event and exit */
	g_usleep(100000);
	while (g_main_pending()) {
		g_main_iteration(TRUE);
	}

	ASSERT_EQ(dbus_test_task_get_state(DBUS_TEST_TASK(zgevent)), DBUS_TEST_TASK_STATE_FINISHED);
	ASSERT_TRUE(dbus_test_task_passed(DBUS_TEST_TASK(zgevent)));

	guint numcalls = 0;
	const DbusTestDbusMockCall * calls = dbus_test_dbus_mock_object_get_method_calls(mock, obj, "InsertEvents", &numcalls, NULL);

	ASSERT_NE(calls, nullptr);
	ASSERT_EQ(numcalls, 1);

	g_object_unref(zgevent);
	g_object_unref(mock);
	g_object_unref(service);
}

static void
zg_state_changed (DbusTestTask * task, DbusTestTaskState state, gpointer user_data)
{
	if (state != DBUS_TEST_TASK_STATE_FINISHED)
		return;

	g_debug("ZG Event Task Finished");

	GMainLoop * mainloop = static_cast<GMainLoop *>(user_data);
	g_main_loop_quit(mainloop);
}

TEST(ZGEvent, TimeoutTest)
{
	GMainLoop * mainloop = g_main_loop_new(NULL, FALSE);
	DbusTestService * service = dbus_test_service_new(NULL);

	DbusTestDbusMock * mock = dbus_test_dbus_mock_new("org.gnome.zeitgeist.Engine");
	DbusTestDbusMockObject * obj = dbus_test_dbus_mock_get_object(mock, "/org/gnome/zeitgeist/log/activity", "org.gnome.zeitgeist.Log", NULL);

	dbus_test_dbus_mock_object_add_method(mock, obj,
		"InsertEvents",
		G_VARIANT_TYPE("a(asaasay)"),
		G_VARIANT_TYPE("au"),
		"time.sleep(6)\n"
		"ret = [ 0 ]",
		NULL);

	dbus_test_service_add_task(service, DBUS_TEST_TASK(mock));

	DbusTestProcess * zgevent = dbus_test_process_new(ZG_EVENT_TOOL);
	dbus_test_process_append_param(zgevent, "close");
	g_setenv("APP_ID", "foo", 1);
	dbus_test_task_set_wait_for(DBUS_TEST_TASK(zgevent), "org.gnome.zeitgeist.Engine");
	dbus_test_task_set_name(DBUS_TEST_TASK(zgevent), "ZGEvent");
	g_signal_connect(G_OBJECT(zgevent), DBUS_TEST_TASK_SIGNAL_STATE_CHANGED, G_CALLBACK(zg_state_changed), mainloop);

	dbus_test_service_add_task(service, DBUS_TEST_TASK(zgevent));

	guint64 start = g_get_monotonic_time();

	dbus_test_service_start_tasks(service);

	g_main_loop_run(mainloop);

	guint64 end = g_get_monotonic_time();

	EXPECT_LT(end - start, 3000 * 1000);

	g_object_unref(zgevent);
	g_object_unref(service);
	g_main_loop_unref(mainloop);
}
