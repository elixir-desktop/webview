defmodule DesktopWebview.Backend do
  @moduledoc """
  `Desktop.Platform` backend backed by the native DesktopWebView host.
  """

  @behaviour Desktop.Platform.Backend
  @behaviour Desktop.Platform.Window
  @behaviour Desktop.Platform.Content
  @behaviour Desktop.Platform.Notification
  @behaviour Desktop.Platform.Media
  @behaviour Desktop.Platform.System

  alias DesktopWebview.{EventBridge, Launcher, Transport}

  @impl true
  def capabilities do
    %{
      window: true,
      content: :native,
      notification: :native,
      menu: :native,
      taskbar: true
    }
  end

  # —— System ——

  @impl true
  def init_env do
    Transport.ensure_started()
    EventBridge.ensure_started()

    case connect_or_launch() do
      :ok ->
        put_webview_backend("DesktopWebView")
        {:edw, :ok}

      {:error, reason} ->
        require Logger
        Logger.error("desktop_webview init_env failed: #{inspect(reason)}")
        {:edw, {:error, reason}}
    end
  end

  defp connect_or_launch do
    cond do
      port = System.get_env("EDW_PORT") ->
        host = System.get_env("EDW_HOST") || "127.0.0.1"

        case Transport.connect(host, String.to_integer(port)) do
          {:ok, _} -> :ok
          other -> other
        end

      Application.get_env(:desktop_webview, :auto_launch, true) ->
        case Launcher.start(
               test_rpc: Application.get_env(:desktop_webview, :test_rpc, false),
               lifetime: Application.get_env(:desktop_webview, :lifetime, :coupled)
             ) do
          {:ok, launcher} ->
            Transport.attach_launcher(launcher)
            # Desktop apps: if the host process dies, halt BEAM (orphan prevention).
            Application.put_env(:desktop_webview, :halt_on_host_exit, true)

            case Transport.connect("127.0.0.1", launcher.listen_port) do
              {:ok, _} -> :ok
              other -> other
            end

          other ->
            other
        end

      true ->
        {:error, :not_connected}
    end
  end

  @impl true
  def subscribe_events do
    # EventBridge owns the Transport subscription and fans out Env/Window/Menu messages.
    EventBridge.ensure_started()
    :ok
  end

  @impl true
  def set_env(_env), do: :ok

  @impl true
  def get_env, do: :ok

  @impl true
  def locale do
    case Transport.call("system.locale", %{}) do
      {:ok, loc} when is_binary(loc) -> String.downcase(loc)
      _ -> nil
    end
  end

  @impl true
  def connect_menu(_object, _command, _callback, _id), do: :ok

  @impl true
  def wx_available?, do: false

  @impl true
  def open_external_url(url) do
    _ = Transport.call("system.open_url", %{"url" => to_string(url)})
    :ok
  end

  @impl true
  def os_description do
    case Transport.call("system.os_description", %{}) do
      {:ok, desc} -> desc
      _ -> nil
    end
  end

  @impl true
  def custom_event(_event, _args), do: :ok

  @impl true
  def activate_event_active?(_event), do: true

  # —— Window ——

  @impl true
  def open(opts) do
    title = Keyword.fetch!(opts, :title)
    {w, h} = Keyword.get(opts, :size, {600, 500})

    params = %{
      "title" => to_string(title),
      "width" => w,
      "height" => h
    }

    params =
      case Keyword.get(opts, :min_size) do
        {mw, mh} -> Map.merge(params, %{"min_width" => mw, "min_height" => mh})
        _ -> params
      end

    params =
      case Keyword.get(opts, :icon) do
        {:icon, id} -> Map.put(params, "icon_id", id)
        id when is_binary(id) -> Map.put(params, "icon_id", id)
        _ -> params
      end

    case Transport.call("window.open", params) do
      {:ok, %{"window_id" => wid, "webview_id" => vid}} ->
        Process.put({:edw_webview, wid}, vid)
        EventBridge.register_window(wid, self())
        {:ok, wid, vid}

      {:ok, other} ->
        {:error, other}

      {:error, reason} ->
        {:error, reason}
    end
  end

  @impl true
  def destroy_frame(frame) do
    _ = Transport.call("window.destroy", %{"window_id" => frame})
    :ok
  end

  @impl true
  def connect(frame, _event, _fun) do
    EventBridge.register_window(frame, self())
    :ok
  end

  @impl true
  def show(frame, opts) do
    _ =
      Transport.call("window.show", %{
        "window_id" => frame,
        "show" => Keyword.get(opts, :show, true)
      })

    :ok
  end

  @impl true
  def hide(frame) do
    _ = Transport.call("window.hide", %{"window_id" => frame})
    :ok
  end

  @impl true
  def set_title(frame, title) do
    _ = Transport.call("window.set_title", %{"window_id" => frame, "title" => to_string(title)})
    :ok
  end

  @impl true
  def set_min_size(frame, {w, h}) do
    _ =
      Transport.call("window.set_min_size", %{"window_id" => frame, "width" => w, "height" => h})

    :ok
  end

  @impl true
  def set_icon(frame, icon) do
    id = icon_id(icon)
    _ = Transport.call("window.set_icon", %{"window_id" => frame, "icon_id" => id})
    :ok
  end

  @impl true
  def set_menubar(frame, menubar) do
    id = menu_id(menubar)
    _ = Transport.call("window.set_menubar", %{"window_id" => frame, "menu_id" => id})
    :ok
  end

  @impl true
  def iconize(frame, iconize) do
    _ = Transport.call("window.iconize", %{"window_id" => frame, "iconize" => iconize})
    :ok
  end

  @impl true
  def shown?(frame) do
    case Transport.call("window.shown", %{"window_id" => frame}) do
      {:ok, true} -> true
      _ -> false
    end
  end

  @impl true
  def active?(frame) do
    case Transport.call("window.active", %{"window_id" => frame}) do
      {:ok, true} -> true
      _ -> false
    end
  end

  @impl true
  def raise_window(nil), do: :ok

  def raise_window(frame) do
    _ = Transport.call("window.raise", %{"window_id" => frame})
    :ok
  end

  @impl true
  def update_apple_menu(title, frame, _menubar) do
    _ =
      Transport.call("menu.set_apple", %{
        "app_name" => to_string(title),
        "window_id" => frame
      })

    :ok
  end

  @impl true
  def new_menubar do
    case Transport.call("menu.create", %{
           "kind" => "menubar",
           "dom" => %{"tag" => "menubar", "attrs" => %{}, "children" => []}
         }) do
      {:ok, %{"menu_id" => id}} -> {:menu, id}
      _ -> {:menu, nil}
    end
  end

  @impl true
  def on_crash_destroy(frame), do: destroy_frame(frame)

  @impl true
  def close_event_veto(_inev), do: :ok

  # —— Content ——

  @impl true
  def attach(frame) do
    # window.open already created a webview; look it up via test API if needed.
    # Desktop.Window calls open then attach — our open returns webview as content handle.
    # When attach is called separately, return nil and rely on stored handle from open.
    case Process.get({:edw_webview, frame}) do
      nil -> nil
      vid -> vid
    end
  end

  @impl true
  def load_url(nil, _frame, url), do: open_external_url(url)

  def load_url(webview, _frame, url) do
    _ = Transport.call("webview.load_url", %{"webview_id" => webview, "url" => to_string(url)})
    :ok
  end

  @impl true
  def reload(nil), do: :ok

  def reload(webview) do
    _ = Transport.call("webview.reload", %{"webview_id" => webview})
    :ok
  end

  @impl true
  def current_url(nil, last_url), do: last_url

  def current_url(webview, last_url) do
    case Transport.call("webview.current_url", %{"webview_id" => webview}) do
      {:ok, url} when is_binary(url) and url != "" -> url
      _ -> last_url
    end
  end

  @impl true
  def content_show(nil, _frame, url, _), do: open_external_url(url)

  def content_show(webview, frame, url, _only_open) do
    if url, do: load_url(webview, frame, url)
    show(frame, show: true)
    raise_window(frame)
    :ok
  end

  @impl true
  def rebuild(nil, _url), do: nil

  def rebuild(frame, url) do
    case Transport.call("webview.rebuild", %{"window_id" => frame}) do
      {:ok, %{"webview_id" => vid}} ->
        if url, do: load_url(vid, frame, url)
        vid

      _ ->
        nil
    end
  end

  @impl true
  def put_webview_backend(name) do
    # Desktop.Env.init/1 calls init_env/0, so a sync GenServer.call here would be a
    # self-call. Defer when we are still inside Env.init.
    case Process.whereis(Desktop.Env) do
      nil ->
        :ok

      pid when pid == self() ->
        spawn(fn -> Desktop.Env.put(:webview_backend, name) end)

      _pid ->
        Desktop.Env.put(:webview_backend, name)
    end

    :ok
  end

  # —— Notification ——

  @impl true
  def new(title, type) do
    {:notification, title, type}
  end

  @impl true
  def notification_show(nil, message, _timeout, title) do
    require Logger
    Logger.notice("NOTIFICATION: #{title}: #{message}")
    :ok
  end

  def notification_show({:notification, default_title, type}, message, timeout, title) do
    register_notification_show(%{
      "title" => to_string(title || default_title),
      "message" => to_string(message),
      "timeout" => timeout,
      "type" => to_string(type)
    })
  end

  def notification_show(id, message, timeout, title) when is_binary(id) do
    register_notification_show(%{
      "id" => id,
      "title" => to_string(title || ""),
      "message" => to_string(message),
      "timeout" => timeout
    })
  end

  defp register_notification_show(params) do
    case Transport.call("notification.show", params) do
      {:ok, %{"notification_id" => nid}} when is_binary(nid) ->
        EventBridge.register_notification(nid, self())
        :ok

      _ ->
        :ok
    end
  end

  @impl true
  def close(nil), do: :ok

  def close({:notification, _, _}), do: :ok

  def close(id) when is_binary(id) do
    _ = Transport.call("notification.close", %{"notification_id" => id})
    :ok
  end

  # —— Media ——

  @impl true
  def load_image(app, path) do
    abs = resolve_priv_path(app, path)

    case Transport.call("icon.create", %{"path" => abs}) do
      {:ok, %{"icon_id" => id}} -> {:ok, {:image, id}}
      {:error, reason} -> {:error, reason}
    end
  end

  @impl true
  def new_icon(app, path) do
    with {:ok, image} <- load_image(app, path), do: new_icon_from(image)
  end

  @impl true
  def new_icon_from({:image, id}), do: {:ok, {:icon, id}}
  def new_icon_from({:icon, id}), do: {:ok, {:icon, id}}
  def new_icon_from(other), do: {:ok, other}

  @impl true
  def default_icon do
    case Transport.call("icon.create", %{}) do
      {:ok, %{"icon_id" => id}} -> {:ok, {:icon, id}}
      _ -> {:ok, {:icon, "default"}}
    end
  end

  defp resolve_priv_path(app, path) when is_binary(path) do
    expanded = Path.expand(path)

    cond do
      Path.type(path) == :absolute ->
        path

      File.exists?(expanded) ->
        expanded

      true ->
        # Desktop.Window passes filenames like "diode.png" (same as wx backend).
        Application.app_dir(app, Path.join("priv", path))
    end
  end

  @impl true
  def media_destroy({:image, id}) do
    _ = Transport.call("icon.destroy", %{"icon_id" => id})
    :ok
  end

  def media_destroy({:icon, id}) do
    _ = Transport.call("icon.destroy", %{"icon_id" => id})
    :ok
  end

  def media_destroy(_), do: :ok

  @impl true
  def object_type({:image, _}), do: :wxImage
  def object_type({:icon, _}), do: :wxIcon
  def object_type(_), do: :unknown

  def create_icon_from_png_base64(b64) when is_binary(b64) do
    case Transport.call("icon.create", %{"png_base64" => b64}) do
      {:ok, %{"icon_id" => id}} -> {:ok, {:icon, id}}
      {:error, reason} -> {:error, reason}
    end
  end

  @doc """
  Enable or disable the webview context menu for the content handle returned by `attach/1`.
  """
  def set_context_menu(webview, enabled) when is_binary(webview) do
    _ =
      Transport.call("webview.set_context_menu", %{
        "webview_id" => webview,
        "enabled" => !!enabled
      })

    :ok
  end

  def set_context_menu(_, _), do: :ok

  defp icon_id({:icon, id}), do: id
  defp icon_id({:image, id}), do: id
  defp icon_id(id) when is_binary(id), do: id
  defp icon_id(_), do: ""

  defp menu_id({:menu, id}), do: id
  defp menu_id(id) when is_binary(id), do: id
  defp menu_id(_), do: ""
end
