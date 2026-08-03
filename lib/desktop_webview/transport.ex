defmodule DesktopWebview.Transport do
  @moduledoc false
  use GenServer

  alias DesktopWebview.Codec

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

  def call(method, params \\ %{}, timeout \\ 5_000) do
    GenServer.call(@name, {:rpc, method, params}, timeout)
  end

  def connect(host, port) do
    GenServer.call(@name, {:connect, host, port}, 10_000)
  end

  def connected? do
    GenServer.call(@name, :connected?)
  end

  def subscribe(pid \\ self()) do
    GenServer.call(@name, {:subscribe, pid})
  end

  def set_permission_handler(fun) when is_function(fun, 1) do
    GenServer.call(@name, {:set_permission_handler, fun})
  end

  def attach_launcher(launcher) do
    GenServer.call(@name, {:attach_launcher, launcher})
  end

  @impl true
  def init(_opts) do
    {:ok,
     %{
       socket: nil,
       next_id: 1,
       pending: %{},
       subscribers: MapSet.new(),
       permission_handler: fn _ -> "ask" end,
       launcher: nil,
       initialized: false
     }}
  end

  @impl true
  def handle_call({:connect, host, port}, _from, state) do
    if state.socket, do: :gen_tcp.close(state.socket)

    case :gen_tcp.connect(
           String.to_charlist(host),
           port,
           [:binary, active: true, packet: 4],
           5_000
         ) do
      {:ok, socket} ->
        state = %{state | socket: socket, pending: %{}, initialized: false}
        id = state.next_id

        :ok =
          send_json(
            socket,
            Codec.request(id, "initialize", %{
              "client" => "desktop_webview",
              "version" => "0.1.0"
            })
          )

        case recv_result(socket, id, 5_000) do
          {:ok, result} ->
            {:reply, {:ok, result}, %{state | next_id: id + 1, initialized: true}}

          {:error, reason} ->
            :gen_tcp.close(socket)
            {:reply, {:error, reason}, %{state | socket: nil}}
        end

      {:error, reason} ->
        {:reply, {:error, reason}, state}
    end
  end

  def handle_call(:connected?, _from, state), do: {:reply, state.socket != nil, state}

  def handle_call({:rpc, _method, _params}, _from, %{socket: nil} = state) do
    {:reply, {:error, :not_connected}, state}
  end

  def handle_call({:rpc, method, params}, from, state) do
    id = state.next_id
    :ok = send_json(state.socket, Codec.request(id, method, params))
    {:noreply, %{state | next_id: id + 1, pending: Map.put(state.pending, id, from)}}
  end

  def handle_call({:subscribe, pid}, _from, state) do
    Process.monitor(pid)
    {:reply, :ok, %{state | subscribers: MapSet.put(state.subscribers, pid)}}
  end

  def handle_call({:set_permission_handler, fun}, _from, state) do
    {:reply, :ok, %{state | permission_handler: fun}}
  end

  def handle_call({:attach_launcher, launcher}, _from, state) do
    {:reply, :ok, %{state | launcher: launcher}}
  end

  @impl true
  def handle_info({:tcp, socket, data}, %{socket: socket} = state) do
    {:noreply, handle_message(Jason.decode!(data), state)}
  end

  def handle_info({:tcp_closed, socket}, %{socket: socket} = state) do
    reply_all(state, {:error, :closed})
    broadcast(state, {:edw_disconnected, :closed})
    {:noreply, %{state | socket: nil, pending: %{}, initialized: false}}
  end

  def handle_info({:tcp_error, socket, reason}, %{socket: socket} = state) do
    reply_all(state, {:error, reason})
    broadcast(state, {:edw_disconnected, reason})
    {:noreply, %{state | socket: nil, pending: %{}, initialized: false}}
  end

  def handle_info({:DOWN, _, :process, pid, _}, state) do
    {:noreply, %{state | subscribers: MapSet.delete(state.subscribers, pid)}}
  end

  def handle_info(_, state), do: {:noreply, state}

  defp handle_message(%{"method" => "permission.request", "id" => id, "params" => params}, state) do
    decision = state.permission_handler.(params)
    :ok = send_json(state.socket, Codec.response(id, %{"decision" => decision}))
    broadcast(state, {:edw_event, "permission.request", params})
    state
  end

  defp handle_message(%{"method" => method, "params" => params} = msg, state)
       when not is_map_key(msg, "id") do
    broadcast(state, {:edw_event, method, params})
    state
  end

  defp handle_message(%{"id" => id} = msg, state) when not is_map_key(msg, "method") do
    case Map.pop(state.pending, normalize_id(id)) do
      {nil, _} ->
        state

      {from, pending} ->
        reply =
          cond do
            Map.has_key?(msg, "result") -> {:ok, msg["result"]}
            Map.has_key?(msg, "error") -> {:error, msg["error"]}
            true -> {:error, :invalid_response}
          end

        GenServer.reply(from, reply)
        %{state | pending: pending}
    end
  end

  defp handle_message(_, state), do: state

  defp send_json(socket, map) do
    :gen_tcp.send(socket, Jason.encode!(map))
  end

  defp recv_result(socket, id, timeout) do
    receive do
      {:tcp, ^socket, data} ->
        msg = Jason.decode!(data)

        cond do
          normalize_id(msg["id"]) == id and Map.has_key?(msg, "result") ->
            {:ok, msg["result"]}

          normalize_id(msg["id"]) == id ->
            {:error, msg["error"]}

          true ->
            # ignore unrelated (shouldn't happen during connect)
            recv_result(socket, id, timeout)
        end

      {:tcp_closed, ^socket} ->
        {:error, :closed}
    after
      timeout ->
        {:error, :timeout}
    end
  end

  defp reply_all(state, reply) do
    Enum.each(state.pending, fn {_, from} -> GenServer.reply(from, reply) end)
  end

  defp broadcast(state, msg), do: Enum.each(state.subscribers, &send(&1, msg))

  defp normalize_id(id) when is_integer(id), do: id
  defp normalize_id(id) when is_float(id), do: trunc(id)
  defp normalize_id(id), do: id
end
