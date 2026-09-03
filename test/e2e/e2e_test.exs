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

    url = fixture_url("media.html")

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

  test "HTML file input fixture exposes chooser semantics" do
    assert {:ok, %{"window_id" => wid, "webview_id" => vid}} =
             Transport.call("window.open", %{
               "title" => "File input",
               "width" => 640,
               "height" => 480
             })

    assert {:ok, true} =
             Transport.call("webview.load_url", %{
               "webview_id" => vid,
               "url" => fixture_url("file_input.html")
             })

    assert :ok = wait_for_file_input(vid)

    # The shared test RPC can inspect DOM state, but cannot drive a native picker.
    assert {:ok, result} =
             Transport.call(
               "test.webview.eval",
               %{
                 "webview_id" => vid,
                 "script" => """
                 JSON.stringify({
                   single: {
                     type: document.querySelector("#single-file").type,
                     multiple: document.querySelector("#single-file").multiple
                   },
                   multiple: {
                     type: document.querySelector("#multiple-files").type,
                     multiple: document.querySelector("#multiple-files").multiple
                   },
                   directory: {
                     type: document.querySelector("#directory-files").type,
                     multiple: document.querySelector("#directory-files").multiple,
                     webkitdirectory: document.querySelector("#directory-files").hasAttribute("webkitdirectory"),
                     directory: document.querySelector("#directory-files").hasAttribute("directory")
                   }
                 })
                 """
               },
               15_000
             )

    assert {:ok,
            %{
              "single" => %{"type" => "file", "multiple" => false},
              "multiple" => %{"type" => "file", "multiple" => true},
              "directory" => %{
                "type" => "file",
                "multiple" => true,
                "webkitdirectory" => true,
                "directory" => true
              }
            }} = Jason.decode(result)

    assert {:ok, true} = Transport.call("window.destroy", %{"window_id" => wid})
  end

  test "window iconize raise hide show does not crash host" do
    assert {:ok, %{"window_id" => wid}} =
             Transport.call("window.open", %{
               "title" => "RaiseMe",
               "width" => 480,
               "height" => 320
             })

    assert {:ok, true} =
             Transport.call("window.iconize", %{"window_id" => wid, "iconize" => true})

    Process.sleep(200)
    assert {:ok, true} = Transport.call("window.raise", %{"window_id" => wid})
    Process.sleep(200)
    assert {:ok, true} = Transport.call("window.hide", %{"window_id" => wid})
    assert {:ok, true} = Transport.call("window.show", %{"window_id" => wid, "show" => true})
    assert {:ok, true} = Transport.call("window.raise", %{"window_id" => wid})
    assert {:ok, "pong"} = Transport.call("test.ping", %{})
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
               "id" => "e2e-note-1",
               "title" => "Hello",
               "message" => "World"
             })

    assert nid == "e2e-note-1"

    # Transport is restarted in setup; restart EventBridge so it resubscribes.
    if pid = Process.whereis(DesktopWebview.EventBridge) do
      Process.exit(pid, :kill)
      Process.sleep(20)
    end

    DesktopWebview.EventBridge.ensure_started()
    DesktopWebview.EventBridge.register_notification(nid, self())

    assert {:ok, true} =
             Transport.call("test.notification.emit_click", %{"notification_id" => nid})

    assert_receive {:edw_notification, ^nid, :click}, 1000

    assert {:ok, true} =
             Transport.call("test.notification.emit_dismiss", %{"notification_id" => nid})

    assert_receive {:edw_notification, ^nid, :dismiss}, 1000

    assert {:ok, true} = Transport.call("notification.close", %{"notification_id" => nid})
    assert {:ok, true} = Transport.call("tray.destroy", %{"tray_id" => tid})
    assert {:ok, true} = Transport.call("menu.destroy", %{"menu_id" => mid})
  end

  test "session reset wipes tray and windows" do
    assert {:ok, %{"window_id" => wid, "webview_id" => _vid}} =
             Transport.call("window.open", %{
               "title" => "ResetMe",
               "width" => 400,
               "height" => 300
             })

    assert {:ok, %{"menu_id" => mid}} =
             Transport.call("menu.create", %{
               "kind" => "popup",
               "dom" => %{"tag" => "menu", "attrs" => %{}, "children" => []}
             })

    assert {:ok, %{"icon_id" => iid}} = Transport.call("icon.create", %{})

    assert {:ok, %{"tray_id" => tid}} =
             Transport.call("tray.create", %{"icon_id" => iid, "menu_id" => mid})

    assert {:ok, windows} = Transport.call("test.window.list", %{})
    assert Enum.any?(windows, &(&1["window_id"] == wid))
    assert {:ok, trays} = Transport.call("test.tray.list", %{})
    assert Enum.any?(trays, &(&1["tray_id"] == tid))

    assert {:ok, true} = Transport.call("test.session.reset", %{})

    assert {:ok, []} = Transport.call("test.window.list", %{})
    assert {:ok, []} = Transport.call("test.tray.list", %{})

    assert {:ok, caps} =
             Transport.call("initialize", %{"client" => "desktop_webview", "version" => "0.1.0"})

    assert caps["protocol_version"] == 1

    assert {:error, _} =
             Transport.call("window.set_title", %{"window_id" => wid, "title" => "gone"})

    assert {:ok, false} = Transport.call("tray.set_icon", %{"tray_id" => tid, "icon_id" => iid})

    assert {:ok, %{"window_id" => wid2}} =
             Transport.call("window.open", %{
               "title" => "AfterReset",
               "width" => 320,
               "height" => 240
             })

    assert wid2 != wid
    assert {:ok, %{"tray_id" => tid2}} = Transport.call("tray.create", %{})
    assert tid2 != tid

    assert {:ok, windows2} = Transport.call("test.window.list", %{})
    assert length(windows2) == 1
    assert {:ok, trays2} = Transport.call("test.tray.list", %{})
    assert length(trays2) == 1
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

    url = fixture_url("media.html")

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
             Transport.call(
               "test.webview.eval",
               %{"webview_id" => vid, "script" => "document.title"},
               15_000
             )

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

  test "default edit menu is installed with copy/paste/cut/selectAll", %{platform: platform} do
    # The macOS host installs a default Edit submenu and exposes it via
    # test.menu.list. The Linux host uses GTK menu bars and does not yet
    # implement test.menu.list; the Edit menu on Linux is required by
    # docs/porting.md and will be added by a follow-up port. On non-macOS
    # we verify the host's documented contract: test.menu.list is a
    # macOS-only test RPC and returns "Unknown test method" elsewhere.
    case platform do
      "macos" ->
        assert {:ok, menus} = Transport.call("test.menu.list", %{})
        assert is_list(menus)

        edit = Enum.find(menus, &(&1["title"] == "Edit"))
        assert edit, "Edit menu missing from NSApp.mainMenu: #{inspect(menus)}"

        by_label = Map.new(edit["items"], fn item -> {item["label"], item} end)

        expected = %{
          "Cut" => {"x", "cut:"},
          "Copy" => {"c", "copy:"},
          "Paste" => {"v", "paste:"},
          "Select All" => {"a", "selectAll:"}
        }

        for {label, {key, action}} <- expected do
          item = Map.get(by_label, label)
          assert item, "Edit menu missing item #{label}; got #{inspect(Map.keys(by_label))}"
          assert item["key"] == key, "#{label} key equivalent expected #{key} got #{item["key"]}"

          assert item["action"] == action,
                 "#{label} action expected #{action} got #{item["action"]}"
        end

      _ ->
        assert {:error, %{"code" => -32601, "message" => "Unknown test method"}} =
                 Transport.call("test.menu.list", %{})
    end
  end

  defp fixture_url(filename) do
    html = File.read!(Path.expand("test/fixtures/#{filename}"))
    "data:text/html;base64," <> Base.encode64(html)
  end

  defp wait_for_file_input(webview_id, attempts \\ 30)

  defp wait_for_file_input(_webview_id, 0), do: :error

  defp wait_for_file_input(webview_id, attempts) do
    ready? =
      case Transport.call("test.webview.eval", %{
             "webview_id" => webview_id,
             "script" =>
               "document.readyState === \"complete\" && " <>
                 "document.querySelector(\"#single-file\") !== null"
           }) do
        {:ok, true} -> true
        _ -> false
      end

    if ready? do
      :ok
    else
      Process.sleep(100)
      wait_for_file_input(webview_id, attempts - 1)
    end
  end
end
