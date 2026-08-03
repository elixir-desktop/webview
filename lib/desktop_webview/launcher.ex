defmodule DesktopWebview.Launcher do
  @moduledoc """
  Starts the native host with `--edw-no-beam` and discovers the listen port from stdout.
  """

  require Logger

  @doc """
  Start the host process.

  Options:
    * `:binary` — path override
    * `:test_rpc` — enable `--edw-test-rpc` (default false)
    * `:port` — `--edw-port` (default 0)
    * `:lifetime` — `:reconnect` | `:coupled`
    * `:extra_args` — additional argv
  """
  def start(opts \\ []) do
    binary = Keyword.get(opts, :binary) || DesktopWebview.Binary.path()

    unless File.regular?(binary) do
      {:error, {:binary_missing, binary}}
    else
      args =
        ["--edw-no-beam", "--edw-port=#{Keyword.get(opts, :port, 0)}"] ++
          test_rpc_args(opts) ++
          lifetime_args(opts) ++
          Keyword.get(opts, :extra_args, [])

      port =
        Port.open(
          {:spawn_executable, String.to_charlist(binary)},
          [
            :binary,
            :exit_status,
            :stderr_to_stdout,
            args: Enum.map(args, &String.to_charlist/1)
          ]
        )

      case await_listening(port, Keyword.get(opts, :timeout, 10_000)) do
        {:ok, listen_port} ->
          # Keep draining host stdout/stderr so WebKit logs cannot fill the pipe.
          drain_pid = spawn_link(fn -> drain_port(port) end)
          true = Port.connect(port, drain_pid)
          {:ok, %{port: port, listen_port: listen_port, binary: binary, drain_pid: drain_pid}}

        {:error, reason} ->
          close_port(port)
          {:error, reason}
      end
    end
  end

  def stop(%{port: port} = launcher) when is_port(port) do
    if pid = Map.get(launcher, :drain_pid) do
      Process.unlink(pid)
      Process.exit(pid, :kill)
    end

    close_port(port)
    :ok
  end

  def stop(_), do: :ok

  defp close_port(port) do
    case Port.info(port) do
      nil -> :ok
      _ -> Port.close(port)
    end
  rescue
    ArgumentError -> :ok
  end

  defp test_rpc_args(opts) do
    if Keyword.get(opts, :test_rpc, false), do: ["--edw-test-rpc"], else: []
  end

  defp lifetime_args(opts) do
    # BEAM-first launches use --edw-no-beam; default to coupled so stopping the
    # VM tears down the host (reconnect is for host-first packaged mode).
    case Keyword.get(opts, :lifetime, :coupled) do
      :reconnect -> ["--edw-lifetime=reconnect"]
      _ -> ["--edw-lifetime=coupled"]
    end
  end

  defp await_listening(port, timeout) do
    deadline = System.monotonic_time(:millisecond) + timeout
    do_await(port, "", deadline)
  end

  defp do_await(port, acc, deadline) do
    if System.monotonic_time(:millisecond) > deadline do
      {:error, :timeout}
    else
      receive do
        {^port, {:data, data}} ->
          acc = acc <> data

          case Regex.run(~r/listening\s+(\d+)/, acc) do
            [_, p] -> {:ok, String.to_integer(p)}
            nil -> do_await(port, acc, deadline)
          end

        {^port, {:exit_status, status}} ->
          {:error, {:exit, status, acc}}
      after
        200 ->
          do_await(port, acc, deadline)
      end
    end
  end

  defp drain_port(port) do
    receive do
      {^port, {:data, _}} ->
        drain_port(port)

      {^port, {:exit_status, _}} ->
        # Host process exited (Quit, crash, or Port.close). When enabled, stop BEAM
        # so a killed UI host cannot leave an orphaned Elixir node.
        if Application.get_env(:desktop_webview, :halt_on_host_exit, false) do
          quit = Application.get_env(:desktop_webview, :quit_fun, &Desktop.Window.quit/0)
          spawn(fn -> quit.() end)
        end

        :ok
    end
  end
end
