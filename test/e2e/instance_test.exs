defmodule DesktopWebview.E2E.InstanceTest do
  use ExUnit.Case, async: false

  @moduletag :e2e

  alias DesktopWebview.{BeamFixture, Binary, Launcher, Transport}

  setup do
    unless Binary.available?() do
      flunk("DesktopWebView binary missing at #{Binary.path()}")
    end

    :ok
  end

  test "second host with URL exits 0 and first gets open_url" do
    {launcher, ctx} = BeamFixture.start_single_instance_host!("si")
    Transport.subscribe(self())

    {out, status} =
      Launcher.oneshot([
        "--edw-no-beam",
        "--edw-config=#{ctx.ini}",
        "--edw-instance-id=#{ctx.id}",
        "ddrive://invite/x"
      ])

    assert status == 0, out
    refute out =~ "listening "
    assert_receive {:edw_event, "event.system.open_url", %{"url" => "ddrive://invite/x"}}, 5_000
    BeamFixture.stop_host(launcher)
  end

  test "empty forwarded argv emits reopen" do
    {launcher, ctx} = BeamFixture.start_single_instance_host!("si")
    Transport.subscribe(self())

    {out, status} =
      Launcher.oneshot([
        "--edw-no-beam",
        "--edw-config=#{ctx.ini}",
        "--edw-instance-id=#{ctx.id}"
      ])

    assert status == 0, out
    refute out =~ "listening "
    assert_receive {:edw_event, "event.system.reopen", _params}, 5_000
    BeamFixture.stop_host(launcher)
  end

  test "instances=multi keeps both hosts up" do
    ctx = BeamFixture.write_instance_ini!(BeamFixture.unique_id("multi"), "multi")
    on_exit(fn -> File.rm_rf(ctx.dir) end)

    {:ok, first} =
      Launcher.start(
        test_rpc: false,
        lifetime: :reconnect,
        extra_args: ["--edw-config=#{ctx.ini}", "--edw-instance-id=#{ctx.instance_id}"]
      )

    {:ok, second} =
      Launcher.start(
        test_rpc: false,
        lifetime: :reconnect,
        extra_args: ["--edw-config=#{ctx.ini}", "--edw-instance-id=#{ctx.instance_id}"]
      )

    on_exit(fn ->
      BeamFixture.stop_host(first)
      BeamFixture.stop_host(second)
    end)

    assert first.listen_port != second.listen_port
    assert is_integer(first.listen_port)
    assert is_integer(second.listen_port)
    BeamFixture.stop_host(first)
    BeamFixture.stop_host(second)
  end

  test "activate respawns dead BEAM child" do
    root = BeamFixture.tmp_dir("edw-activate-respawn")
    instance_id = BeamFixture.unique_id("si-respawn")

    fx =
      BeamFixture.write_restart_fixture!(root,
        stay_up: true,
        instances: "single",
        instance_id: instance_id
      )

    {:ok, launcher} =
      Launcher.start(
        no_beam: false,
        test_rpc: false,
        lifetime: :reconnect,
        extra_args: ["--edw-config=#{fx.ini}", "--edw-instance-id=#{instance_id}"]
      )

    on_exit(fn ->
      BeamFixture.stop_host(launcher)
      File.rm_rf(root)
    end)

    assert BeamFixture.wait_until(fn ->
             BeamFixture.count_lines(Path.join(root, "starts.log")) >= 1
           end)

    before = BeamFixture.count_lines(Path.join(root, "starts.log"))
    BeamFixture.kill_direct_children!(launcher.os_pid)

    {out, status} =
      Launcher.oneshot([
        "--edw-no-beam",
        "--edw-config=#{fx.ini}",
        "--edw-instance-id=#{instance_id}"
      ])

    assert status == 0, out

    assert BeamFixture.wait_until(fn ->
             BeamFixture.count_lines(Path.join(root, "starts.log")) > before
           end),
           "expected activate to respawn dead BEAM; starts=#{inspect(File.read(Path.join(root, "starts.log")))}"

    assert match?([_ | _], Port.info(launcher.port))
  end
end
