defmodule DesktopWebview.E2E.RpcTest do
  use ExUnit.Case, async: false

  @moduletag :e2e

  alias DesktopWebview.{BeamFixture, Binary, Launcher, Transport}

  setup do
    unless Binary.available?() do
      flunk("DesktopWebView binary missing at #{Binary.path()}")
    end

    :ok
  end

  test "inspects 1+1 as 2" do
    {launcher, ctx} = start_single_host!()

    {out, status} =
      Launcher.oneshot([
        "--edw-rpc",
        "1+1",
        "--edw-config=#{ctx.ini}",
        "--edw-instance-id=#{ctx.id}"
      ])

    assert status == 0, out
    assert String.split(String.trim(out), "\n", trim: true) |> Enum.any?(&(&1 == "2"))
    stop_host(launcher)
  end

  test "evaluates a module on the connected client" do
    {launcher, ctx} = start_single_host!()

    {out, status} =
      Launcher.oneshot([
        "--edw-rpc",
        "DesktopWebview.Binary.available?()",
        "--edw-config=#{ctx.ini}",
        "--edw-instance-id=#{ctx.id}"
      ])

    assert status == 0, out
    assert String.trim(out) |> String.split("\n", trim: true) |> Enum.any?(&(&1 == "true"))
    stop_host(launcher)
  end

  test "fails when no single-instance host is running" do
    ctx = BeamFixture.write_instance_ini!(BeamFixture.unique_id("rpc-missing"))
    ini = ctx.ini
    id = ctx.instance_id

    {_out, status} =
      Launcher.oneshot([
        "--edw-rpc",
        "1+1",
        "--edw-config=#{ini}",
        "--edw-instance-id=#{id}"
      ])

    assert status != 0
  end

  test "rejects --edw-rpc together with --edw-recover" do
    {out, status} =
      Launcher.oneshot(["--edw-rpc", "1+1", "--edw-recover"])

    assert status != 0
    refute out =~ "listening "
    assert out =~ "mutually exclusive"
  end

  defp start_single_host! do
    ctx = BeamFixture.write_instance_ini!(BeamFixture.unique_id("rpc"))

    {:ok, launcher} =
      Launcher.start(
        test_rpc: false,
        lifetime: :reconnect,
        extra_args: ["--edw-config=#{ctx.ini}", "--edw-instance-id=#{ctx.instance_id}"]
      )

    if pid = Process.whereis(Transport), do: GenServer.stop(pid, :normal, 1000)
    {:ok, _} = Transport.start_link([])
    assert {:ok, _caps} = Transport.connect("127.0.0.1", launcher.listen_port)

    on_exit(fn ->
      stop_host(launcher)
      File.rm_rf(ctx.dir)
    end)

    {launcher, %{id: ctx.instance_id, ini: ctx.ini, dir: ctx.dir}}
  end

  defp stop_host(launcher) do
    Launcher.stop(launcher)
  rescue
    _ -> :ok
  end
end
