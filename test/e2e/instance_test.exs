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
    {launcher, ctx} = start_single_host!()
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
    stop_host(launcher)
  end

  test "empty forwarded argv emits reopen" do
    {launcher, ctx} = start_single_host!()
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
    stop_host(launcher)
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
      stop_host(first)
      stop_host(second)
    end)

    assert first.listen_port != second.listen_port
    assert is_integer(first.listen_port)
    assert is_integer(second.listen_port)
    stop_host(first)
    stop_host(second)
  end

  defp start_single_host! do
    ctx = BeamFixture.write_instance_ini!(BeamFixture.unique_id("si"))

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
