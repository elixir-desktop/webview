defmodule DesktopWebview.E2E.RpcTest do
  use ExUnit.Case, async: false

  @moduletag :e2e

  alias DesktopWebview.{BeamFixture, Binary, Launcher}

  setup do
    unless Binary.available?() do
      flunk("DesktopWebView binary missing at #{Binary.path()}")
    end

    :ok
  end

  defp rpc_beam_dir! do
    cookie = :edw_e2e_cookie
    node = BeamFixture.ensure_distributed!(cookie)
    beam_dir = BeamFixture.tmp_dir("edw-rpc")
    on_exit(fn -> File.rm_rf(beam_dir) end)

    BeamFixture.write_rpc_release!(beam_dir,
      node: node,
      cookie: to_string(cookie)
    )

    beam_dir
  end

  test "inspects 1+1 as 2" do
    beam_dir = rpc_beam_dir!()

    {out, status} =
      Launcher.oneshot([
        "--edw-rpc",
        "1+1",
        "--edw-beam-path=#{beam_dir}"
      ])

    assert status == 0, out
    assert String.split(String.trim(out), "\n", trim: true) |> Enum.any?(&(&1 == "2"))
  end

  test "evaluates a module on the test node" do
    beam_dir = rpc_beam_dir!()

    {out, status} =
      Launcher.oneshot([
        "--edw-rpc",
        "DesktopWebview.Binary.available?()",
        "--edw-beam-path=#{beam_dir}"
      ])

    assert status == 0, out
    assert String.trim(out) |> String.split("\n", trim: true) |> Enum.any?(&(&1 == "true"))
  end

  test "fails when the node name is wrong" do
    beam_dir = rpc_beam_dir!()

    File.write!(
      Path.join(beam_dir, "releases/0.1.0/vm.args"),
      "-name missing_edw_rpc@127.0.0.1\n-setcookie edw_e2e_cookie\n"
    )

    {_out, status} =
      Launcher.oneshot([
        "--edw-rpc",
        "1+1",
        "--edw-beam-path=#{beam_dir}"
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
end
