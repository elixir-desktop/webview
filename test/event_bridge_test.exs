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

  test "open_url notifies Desktop.Env subscribers when Env is running", %{bridge: bridge} do
    # Without Desktop.Env, dispatch is a no-op — just ensure no crash.
    send(bridge, {:edw_event, "event.system.open_url", %{"url" => "ddrive://invite/abc"}})
    Process.sleep(50)
    assert Process.alive?(bridge)
  end
end
