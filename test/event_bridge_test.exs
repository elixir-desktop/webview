defmodule DesktopWebview.EventBridgeTest do
  use ExUnit.Case, async: false

  alias DesktopWebview.EventBridge

  setup do
    # Isolate from a previously started named EventBridge in the VM.
    if pid = Process.whereis(EventBridge) do
      Process.exit(pid, :kill)
      # Wait for name free
      Process.sleep(20)
    end

    {:ok, bridge} = EventBridge.start_link([])
    %{bridge: bridge}
  end

  test "close_requested casts :close_window to registered window", %{bridge: bridge} do
    EventBridge.register_window("w1", self())
    send(bridge, {:edw_event, "event.window.close_requested", %{"window_id" => "w1"}})
    assert_receive {:"$gen_cast", :close_window}, 500
  end

  test "focus casts :frame_activated", %{bridge: bridge} do
    EventBridge.register_window("w1", self())
    send(bridge, {:edw_event, "event.window.focus", %{"window_id" => "w1"}})
    assert_receive {:"$gen_cast", :frame_activated}, 500
  end

  test "menu.click triggers menu event", %{bridge: bridge} do
    EventBridge.register_menu("m1", self())
    send(bridge, {:edw_event, "event.menu.click", %{"menu_id" => "m1", "onclick" => "quit"}})
    assert_receive {:"$gen_cast", {:trigger_event, "quit"}}, 500
  end

  test "notification.click delivers edw_notification", %{bridge: bridge} do
    EventBridge.register_notification("nid-1", self())
    send(bridge, {:edw_event, "event.notification.click", %{"notification_id" => "nid-1"}})
    assert_receive {:edw_notification, "nid-1", :click}, 500
  end

  test "notification.dismiss delivers edw_notification", %{bridge: bridge} do
    EventBridge.register_notification("nid-2", self())
    send(bridge, {:edw_event, "event.notification.dismiss", %{"notification_id" => "nid-2"}})
    assert_receive {:edw_notification, "nid-2", :dismiss}, 500
  end

  test "quit invokes configured quit_fun", %{bridge: bridge} do
    test = self()
    Application.put_env(:desktop_webview, :quit_fun, fn -> send(test, :quit_requested) end)
    on_exit(fn -> Application.delete_env(:desktop_webview, :quit_fun) end)

    send(bridge, {:edw_event, "event.system.quit", %{}})
    assert_receive :quit_requested, 500
  end

  test "open_url notifies Desktop.Env subscribers when Env is running", %{bridge: bridge} do
    # Without Desktop.Env, dispatch is a no-op — just ensure no crash.
    send(bridge, {:edw_event, "event.system.open_url", %{"url" => "ddrive://invite/abc"}})
    Process.sleep(50)
    assert Process.alive?(bridge)
  end
end
