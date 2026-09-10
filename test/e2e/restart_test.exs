defmodule DesktopWebview.E2E.RestartTest do
  use ExUnit.Case, async: false

  @moduletag :e2e

  alias DesktopWebview.{BeamFixture, Binary, Launcher}

  setup do
    unless Binary.available?() do
      flunk("DesktopWebView binary missing at #{Binary.path()}")
    end

    :ok
  end

  test "runs Mix eval without start or listening" do
    root = BeamFixture.tmp_dir("edw-recover")
    on_exit(fn -> File.rm_rf(root) end)

    fx =
      BeamFixture.write_restart_fixture!(root,
        lifetime_ini: "recovery_script = #{root}/recovery.exs"
      )

    {out, status} =
      Launcher.oneshot([
        "--edw-recover",
        "--edw-config=#{fx.ini}"
      ])

    assert status == 0,
           "status=#{status} out=#{inspect(out)} calls=#{inspect(File.read(Path.join(root, "calls.log")))} eval=#{inspect(File.read(Path.join(root, "eval.log")))}"

    refute out =~ "listening "
    assert File.exists?(Path.join(root, "recovered"))
    assert BeamFixture.count_lines(Path.join(root, "starts.log")) == 0
    assert BeamFixture.count_lines(Path.join(root, "eval.log")) == 1
  end

  test "fails when recovery_script is missing" do
    {_out, status} = Launcher.oneshot(["--edw-recover"])
    assert status != 0
  end

  test "runs recovery eval after three startup crashes then start succeeds" do
    root = BeamFixture.tmp_dir("edw-restart")

    fx =
      BeamFixture.write_restart_fixture!(root,
        lifetime_ini: """
        recovery_after = 3
        recovery_script = #{root}/recovery.exs
        """
      )

    {:ok, launcher} =
      Launcher.start(
        no_beam: false,
        test_rpc: false,
        extra_args: ["--edw-config=#{fx.ini}"]
      )

    on_exit(fn ->
      stop_host(launcher)
      kill_fixture_children(root)
      File.rm_rf(root)
    end)

    assert BeamFixture.wait_until(fn ->
             File.exists?(Path.join(root, "recovered")) and
               BeamFixture.count_lines(Path.join(root, "starts.log")) >= 4
           end),
           "expected recovery then a successful start; starts=#{inspect(File.read(Path.join(root, "starts.log")))} eval=#{inspect(File.read(Path.join(root, "eval.log")))} calls=#{inspect(File.read(Path.join(root, "calls.log")))}"

    assert BeamFixture.count_lines(Path.join(root, "eval.log")) == 1
  end

  test "stops after three crashes when max attempts is 3 and no recovery script" do
    root = BeamFixture.tmp_dir("edw-max")
    on_exit(fn -> File.rm_rf(root) end)

    fx =
      BeamFixture.write_restart_fixture!(root,
        always_fail: true,
        lifetime_ini: """
        restart_max_attempts = 3
        """
      )

    result =
      Launcher.start(
        no_beam: false,
        test_rpc: false,
        timeout: 8_000,
        extra_args: ["--edw-config=#{fx.ini}"]
      )

    case result do
      {:error, {:exit, status, _acc}} ->
        assert status != 0

      {:ok, launcher} ->
        on_exit(fn -> stop_host(launcher) end)
        assert BeamFixture.wait_until(fn -> not host_alive?(launcher) end, 8_000)

      other ->
        flunk("unexpected launcher result: #{inspect(other)}")
    end

    assert BeamFixture.count_lines(Path.join(root, "starts.log")) == 3
    refute File.exists?(Path.join(root, "eval.log"))
  end

  defp host_alive?(%{port: port}) do
    match?([_ | _], Port.info(port))
  end

  defp stop_host(%{os_pid: os_pid} = launcher) when is_integer(os_pid) do
    Launcher.stop(launcher)

    case :os.type() do
      {:win32, _} ->
        System.cmd("taskkill", ["/F", "/PID", Integer.to_string(os_pid)], stderr_to_stdout: true)

      _ ->
        System.cmd("kill", ["-9", Integer.to_string(os_pid)], stderr_to_stdout: true)
    end
  end

  defp stop_host(launcher), do: Launcher.stop(launcher)

  defp kill_fixture_children(root) do
    case :os.type() do
      {:win32, _} ->
        :ok

      _ ->
        System.cmd("pkill", ["-f", root], stderr_to_stdout: true)
    end
  end
end
