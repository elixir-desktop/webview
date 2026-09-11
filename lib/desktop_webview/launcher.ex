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
    * `:no_beam` — pass `--edw-no-beam` (default true)
    * `:timeout` — wait for `listening` (ms)
  """
  def start(opts \\ []) do
    binary = Keyword.get(opts, :binary) || DesktopWebview.Binary.path()

    unless File.regular?(binary) do
      {:error, {:binary_missing, binary}}
    else
      args =
        no_beam_args(opts) ++
          ["--edw-port=#{Keyword.get(opts, :port, 0)}"] ++
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

      os_pid =
        case Port.info(port, :os_pid) do
          {:os_pid, pid} -> pid
          _ -> nil
        end

      case await_listening(port, Keyword.get(opts, :timeout, 10_000)) do
        {:ok, listen_port} ->
          # Drain without linking to the caller. A link would exit the caller
          # when the drain stops, and a stored drain pid must never be killed
          # later — ExUnit can reuse that pid for the next test.
          drain_pid = spawn(fn -> drain_port(port) end)
          true = Port.connect(port, drain_pid)

          {:ok,
           %{
             port: port,
             listen_port: listen_port,
             binary: binary,
             drain_pid: drain_pid,
             os_pid: os_pid
           }}

        {:error, reason} ->
          close_port(port)
          {:error, reason}
      end
    end
  end

  @doc """
  Run the host as a one-shot CLI (`--edw-rpc` / `--edw-recover`). Does not wait for `listening`.
  """
  def oneshot(args, opts \\ []) when is_list(args) do
    binary = Keyword.get(opts, :binary) || DesktopWebview.Binary.path()
    System.cmd(binary, args, stderr_to_stdout: true)
  end

  def stop(%{port: port} = launcher) when is_port(port) do
    close_port(port)
    terminate_os(Map.get(launcher, :os_pid))
    :ok
  end

  def stop(_), do: :ok

  defp terminate_os(os_pid) when is_integer(os_pid) and os_pid > 0 do
    case :os.type() do
      {:win32, _} ->
        System.cmd("taskkill", ["/PID", Integer.to_string(os_pid), "/T", "/F"],
          stderr_to_stdout: true
        )

      _ ->
        System.cmd("kill", ["-TERM", Integer.to_string(os_pid)], stderr_to_stdout: true)
    end
  rescue
    _ -> :ok
  end

  defp terminate_os(_), do: :ok

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

  defp no_beam_args(opts) do
    if Keyword.get(opts, :no_beam, true), do: ["--edw-no-beam"], else: []
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
