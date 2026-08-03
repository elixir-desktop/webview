defmodule DesktopWebview.EventBridge do
  @moduledoc """
  Translates host `event.*` notifications into `Desktop.Env` / `Desktop.Window` /
  `Desktop.Menu` messages so apps keep working without raw `{:edw_event, ...}` handling.
  """
  use GenServer

  alias DesktopWebview.Transport

  @name __MODULE__

  def start_link(opts \\ []) do
    GenServer.start_link(__MODULE__, opts, name: @name)
  end

  def ensure_started do
    case Process.whereis(@name) do
      nil ->
        {:ok, _} = start_link([])
        :ok

      _ ->
        :ok
    end
  end

  def register_window(window_id, pid) when is_binary(window_id) and is_pid(pid) do
    ensure_started()
    GenServer.cast(@name, {:register_window, window_id, pid})
  end

  def register_menu(menu_id, pid) when is_binary(menu_id) and is_pid(pid) do
    ensure_started()
    GenServer.cast(@name, {:register_menu, menu_id, pid})
  end

  def register_notification(notification_id, pid)
      when is_binary(notification_id) and is_pid(pid) do
    ensure_started()
    GenServer.cast(@name, {:register_notification, notification_id, pid})
  end

  @impl true
  def init(_opts) do
    Transport.ensure_started()
    Transport.subscribe(self())

    {:ok,
     %{
       windows: %{},
       menus: %{},
       notifications: %{}
     }}
  end

  @impl true
  def handle_cast({:register_window, window_id, pid}, state) do
    Process.monitor(pid)
    {:noreply, %{state | windows: Map.put(state.windows, window_id, pid)}}
  end

  def handle_cast({:register_menu, menu_id, pid}, state) do
    Process.monitor(pid)
    {:noreply, %{state | menus: Map.put(state.menus, menu_id, pid)}}
  end

  def handle_cast({:register_notification, notification_id, pid}, state) do
    Process.monitor(pid)
    {:noreply, %{state | notifications: Map.put(state.notifications, notification_id, pid)}}
  end

  @impl true
  def handle_info({:edw_event, method, params}, state) do
    {:noreply, dispatch(method, params, state)}
  end

  def handle_info({:DOWN, _ref, :process, pid, _}, state) do
    {:noreply,
     %{
       state
       | windows: Map.reject(state.windows, fn {_k, v} -> v == pid end),
         menus: Map.reject(state.menus, fn {_k, v} -> v == pid end),
         notifications: Map.reject(state.notifications, fn {_k, v} -> v == pid end)
     }}
  end

  def handle_info(_other, state), do: {:noreply, state}

  defp dispatch("event.window.close_requested", %{"window_id" => wid}, state) do
    if pid = Map.get(state.windows, wid), do: GenServer.cast(pid, :close_window)
    state
  end

  defp dispatch("event.window.focus", %{"window_id" => wid}, state) do
    if pid = Map.get(state.windows, wid), do: GenServer.cast(pid, :frame_activated)
    state
  end

  defp dispatch("event.system.open_url", params, state) do
    url = params["url"] || params["path"]

    if is_binary(url) and Process.whereis(Desktop.Env) do
      Desktop.Env.notify_subscribers({:open_url, [url]})
    end

    state
  end

  defp dispatch("event.system.open_file", params, state) do
    path = params["path"] || params["url"]

    if is_binary(path) and Process.whereis(Desktop.Env) do
      Desktop.Env.notify_subscribers({:open_file, [path]})
    end

    state
  end

  defp dispatch("event.system.reopen", _params, state) do
    if env = Process.whereis(Desktop.Env) do
      send(env, {:reopen_app, []})
    end

    state
  end

  defp dispatch("event.menu.click", %{"menu_id" => menu_id, "onclick" => onclick}, state) do
    if pid = Map.get(state.menus, menu_id) do
      GenServer.cast(pid, {:trigger_event, onclick})
    end

    state
  end

  defp dispatch("event.notification.click", %{"notification_id" => id}, state) do
    notify_notification(state, id, :click)
  end

  defp dispatch("event.notification.dismiss", %{"notification_id" => id}, state) do
    notify_notification(state, id, :dismiss)
  end

  defp dispatch("event.webview.new_window", params, state) do
    url = params["url"]

    if is_binary(url) do
      _ = Transport.call("system.open_url", %{"url" => url})
    end

    state
  end

  defp dispatch(_method, _params, state), do: state

  defp notify_notification(state, id, action) do
    if pid = Map.get(state.notifications, id) do
      send(pid, {:edw_notification, id, action})
    end

    state
  end
end
