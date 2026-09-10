defmodule DesktopWebview.BeamFixture do
  @moduledoc false

  def tmp_dir(prefix) do
    dir =
      Path.join(
        System.tmp_dir!(),
        "#{prefix}-#{System.unique_integer([:positive])}"
      )

    File.mkdir_p!(dir)
    dir
  end

  def erl_call_path do
    root = :code.root_dir() |> List.to_string()

    [
      Path.join(root, "erts-*/bin/erl_call"),
      Path.join(root, "lib/erl_interface-*/bin/erl_call")
    ]
    |> Enum.flat_map(&Path.wildcard/1)
    |> List.first()
  end

  def erts_dir do
    root = :code.root_dir() |> List.to_string()
    Path.wildcard(Path.join(root, "erts-*")) |> List.first()
  end

  def ensure_distributed!(cookie) when is_atom(cookie) do
    unless Node.alive?() do
      ensure_epmd!()
      name = :"edw_e2e_#{System.unique_integer([:positive])}@127.0.0.1"
      start_longnames!(name)
    end

    Node.set_cookie(cookie)
    Node.self()
  end

  defp ensure_epmd! do
    case :os.find_executable(~c"epmd") do
      false ->
        raise "epmd not found on PATH"

      path ->
        start_epmd(List.to_string(path))
    end
  end

  defp start_epmd(path) do
    case :os.type() do
      {:win32, _} ->
        # Windows `epmd -daemon` stays attached; do not wait on System.cmd.
        _port =
          Port.open(
            {:spawn_executable, String.to_charlist(path)},
            [:hide, args: [~c"-daemon"]]
          )

        Process.sleep(200)

      _ ->
        System.cmd(path, ["-daemon"], stderr_to_stdout: true)
    end
  end

  defp start_longnames!(name, attempts \\ 20) do
    case Node.start(name, :longnames) do
      {:ok, _} ->
        :ok

      {:error, {:already_started, _}} ->
        :ok

      {:error, _reason} when attempts > 1 ->
        Process.sleep(50)
        start_longnames!(name, attempts - 1)

      {:error, reason} ->
        raise "Node.start(#{inspect(name)}) failed: #{inspect(reason)}"
    end
  end

  def write_rpc_release!(beam_dir, opts) do
    node = Keyword.fetch!(opts, :node)
    cookie = Keyword.fetch!(opts, :cookie)
    File.mkdir_p!(Path.join(beam_dir, "releases/0.1.0"))
    File.mkdir_p!(Path.join(beam_dir, "bin"))
    File.write!(Path.join(beam_dir, "releases/COOKIE"), "#{cookie}\n")

    File.write!(
      Path.join(beam_dir, "releases/start_erl.data"),
      "0.0 0.1.0\n"
    )

    File.write!(
      Path.join(beam_dir, "releases/0.1.0/vm.args"),
      "-name #{node}\n-setcookie #{cookie}\n"
    )

    beam_dir
  end

  def write_restart_fixture!(root, opts \\ []) do
    always_fail? = Keyword.get(opts, :always_fail, false)
    bin = Path.join(root, "bin")
    File.mkdir_p!(bin)
    script = Path.join(bin, "edw_beam")
    bat = Path.join(bin, "edw_beam.bat")
    File.write!(script, unix_stub())
    File.chmod!(script, 0o755)
    File.write!(bat, windows_stub())

    recovery = Path.join(root, "recovery.exs")

    File.write!(recovery, """
    File.write!(Path.join(Path.dirname(__ENV__.file), "recovered"), "ok\\n")
    """)

    if always_fail?, do: File.write!(Path.join(root, "always_fail"), "1\n")

    ini = Path.join(root, "DesktopWebView.ini")

    extra_lifetime = Keyword.get(opts, :lifetime_ini, "")

    File.write!(ini, """
    [beam]
    path = #{root}
    app_name = edw_beam
    args = start
    working_dir = #{root}

    [lifetime]
    mode = reconnect
    restart_beam = true
    restart_backoff_ms = 50
    #{extra_lifetime}
    """)

    %{root: root, ini: ini, recovery: recovery, script: script}
  end

  def count_lines(path) do
    case File.read(path) do
      {:ok, body} ->
        body |> String.replace("\r", "") |> String.split("\n", trim: true) |> length()

      {:error, _} ->
        0
    end
  end

  def wait_until(fun, timeout_ms \\ 15_000) do
    deadline = System.monotonic_time(:millisecond) + timeout_ms
    do_wait(fun, deadline)
  end

  defp do_wait(fun, deadline) do
    if fun.() do
      true
    else
      if System.monotonic_time(:millisecond) > deadline do
        false
      else
        Process.sleep(50)
        do_wait(fun, deadline)
      end
    end
  end

  defp unix_stub do
    """
    #!/bin/sh
    ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
    CMD="${1:-}"
    shift || true
    case "$CMD" in
      start)
        echo start >> "$ROOT/starts.log"
        if [ -f "$ROOT/always_fail" ]; then
          exit 1
        fi
        if [ -f "$ROOT/recovered" ]; then
          while true; do sleep 3600; done
        fi
        exit 1
        ;;
      eval)
        echo "eval $*" >> "$ROOT/eval.log"
        elixir -e "$*"
        ;;
      *)
        echo "unknown command: $CMD" >&2
        exit 1
        ;;
    esac
    """
  end

  defp windows_stub do
    """
    @echo off
    set ROOT=%~dp0..
    echo cmd %*>> "%ROOT%\calls.log"
    if /I "%~1"=="eval" (
      echo eval %~2>> "%ROOT%\\eval.log"
      elixir -e "%~2"
      exit /b %ERRORLEVEL%
    )
    echo start>> "%ROOT%\\starts.log"
    if exist "%ROOT%\\always_fail" exit /b 1
    if exist "%ROOT%\\recovered" (
      ping -n 3600 127.0.0.1 >nul
      exit /b 0
    )
    exit /b 1
    """
  end
end
