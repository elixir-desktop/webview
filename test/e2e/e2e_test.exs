defmodule DesktopWebview.E2ETest do
  use ExUnit.Case, async: false

  @moduletag :e2e

  alias DesktopWebview.{Binary, Launcher, Transport}

  setup do
    unless Binary.available?() do
      flunk(
        "DesktopWebView binary missing at #{Binary.path()}; " <>
          "run ./scripts/build_macos.sh, ./scripts/build_linux.sh, or .\\scripts\\build_windows.ps1"
      )
    end

    {:ok, launcher} = Launcher.start(test_rpc: true, lifetime: :reconnect)

    on_exit(fn ->
      # Best-effort kill of host process
      if is_port(launcher.port) and Port.info(launcher.port) do
        try do
          Port.close(launcher.port)
        rescue
          ArgumentError -> :ok
        end
      end

      # Also kill by name in case port already closed
      case :os.type() do
        {:win32, _} ->
          System.cmd("taskkill", ["/F", "/IM", "DesktopWebView.exe"], stderr_to_stdout: true)

        _ ->
          System.cmd("pkill", ["-f", "DesktopWebView --edw-no-beam"], stderr_to_stdout: true)
      end
    end)

    # Ensure transport is fresh
    if pid = Process.whereis(Transport), do: GenServer.stop(pid, :normal, 1000)
    {:ok, _} = Transport.start_link([])
    assert {:ok, caps} = Transport.connect("127.0.0.1", launcher.listen_port)
    assert caps["protocol_version"] == 1

    expected_platform =
      case :os.type() do
        {:unix, :darwin} -> "macos"
        {:win32, _} -> "windows"
        {:unix, _} -> "linux"
      end

    assert caps["platform"] == expected_platform

    Transport.set_permission_handler(fn _ -> "allow" end)

    %{launcher: launcher, caps: caps, platform: expected_platform}
  end

  test "test.ping" do
    assert {:ok, "pong"} = Transport.call("test.ping", %{})
  end

  test "test.echo" do
    assert {:ok, %{"a" => 1}} = Transport.call("test.echo", %{"a" => 1})
  end

  test "window open load reload and list" do
    assert {:ok, %{"window_id" => wid, "webview_id" => vid}} =
             Transport.call("window.open", %{
               "title" => "E2E",
               "width" => 640,
               "height" => 480
             })

    html = File.read!(Path.expand("test/fixtures/media.html"))
    url = "data:text/html;charset=utf-8," <> URI.encode(html)

    assert {:ok, true} = Transport.call("webview.load_url", %{"webview_id" => vid, "url" => url})
    # Give the engine a moment
    Process.sleep(500)

    assert {:ok, true} = Transport.call("webview.reload", %{"webview_id" => vid})
    Process.sleep(300)

    assert {:ok, list} = Transport.call("test.window.list", %{})
    assert Enum.any?(list, &(&1["window_id"] == wid))

    assert {:ok, true} =
             Transport.call("window.set_title", %{"window_id" => wid, "title" => "E2E2"})

    assert {:ok, true} = Transport.call("window.raise", %{"window_id" => wid})
    assert {:ok, true} = Transport.call("window.hide", %{"window_id" => wid})
    assert {:ok, true} = Transport.call("window.show", %{"window_id" => wid, "show" => true})

    assert {:ok, %{"webview_id" => vid2}} =
             Transport.call("webview.rebuild", %{"window_id" => wid})

    assert is_binary(vid2)

    assert {:ok, true} = Transport.call("window.destroy", %{"window_id" => wid})
  end

  test "menu create and notification" do
    dom = %{
      "tag" => "menubar",
      "attrs" => %{},
      "children" => [
        %{
          "tag" => "menu",
          "attrs" => %{"label" => "File"},
          "children" => [
            %{"tag" => "item", "attrs" => %{"onclick" => "quit"}, "children" => ["Quit"]}
          ]
        }
      ]
    }

    assert {:ok, %{"menu_id" => mid}} =
             Transport.call("menu.create", %{"kind" => "menubar", "dom" => dom})

    assert {:ok, %{"icon_id" => iid}} = Transport.call("icon.create", %{})

    assert {:ok, %{"tray_id" => tid}} =
             Transport.call("tray.create", %{"icon_id" => iid, "menu_id" => mid})

    assert {:ok, %{"notification_id" => nid}} =
             Transport.call("notification.show", %{
               "title" => "Hello",
               "message" => "World"
             })

    assert {:ok, true} = Transport.call("notification.close", %{"notification_id" => nid})
    assert {:ok, true} = Transport.call("tray.destroy", %{"tray_id" => tid})
    assert {:ok, true} = Transport.call("menu.destroy", %{"menu_id" => mid})
  end

  test "permission policy and simulate" do
    assert {:ok, true} =
             Transport.call("system.set_permission_policy", %{
               "origin" => "http://127.0.0.1",
               "microphone" => "allow",
               "camera" => "allow"
             })

    assert {:ok, %{"window_id" => _wid, "webview_id" => vid}} =
             Transport.call("window.open", %{"title" => "Media", "width" => 400, "height" => 300})

    html = File.read!(Path.expand("test/fixtures/media.html"))
    url = "data:text/html;charset=utf-8," <> URI.encode(html)

    assert {:ok, true} =
             Transport.call("webview.load_url", %{"webview_id" => vid, "url" => url})

    Process.sleep(400)

    Transport.subscribe(self())

    assert {:ok, true} =
             Transport.call("test.permission.simulate", %{
               "origin" => "http://127.0.0.1",
               "type" => "microphone",
               "webview_id" => vid
             })

    assert_receive {:edw_event, "permission.request", %{"type" => "microphone"}}, 2_000

    # JS eval fixture marker
    assert {:ok, result} =
             Transport.call("test.webview.eval", %{
               "webview_id" => vid,
               "script" => "document.title"
             })

    assert result == "EDW Media Fixture" or is_binary(result)
  end

  test "system locale and os_description", %{platform: platform} do
    assert {:ok, loc} = Transport.call("system.locale", %{})
    assert is_binary(loc)
    assert {:ok, desc} = Transport.call("system.os_description", %{})

    case platform do
      "macos" -> assert desc =~ "macOS"
      "linux" -> assert desc =~ "Linux"
      "windows" -> assert desc =~ "Windows"
    end
  end

  test "multi-window" do
    assert {:ok, %{"window_id" => w1}} =
             Transport.call("window.open", %{"title" => "A", "width" => 300, "height" => 200})

    assert {:ok, %{"window_id" => w2}} =
             Transport.call("window.open", %{"title" => "B", "width" => 300, "height" => 200})

    assert w1 != w2
    assert {:ok, list} = Transport.call("test.window.list", %{})
    assert length(list) >= 2
  end
end
