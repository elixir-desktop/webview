defmodule DesktopWebview.CodecTest do
  use ExUnit.Case, async: true

  alias DesktopWebview.Codec

  test "encode/decode roundtrip" do
    map = Codec.request(1, "test.ping", %{})
    packet = Codec.encode(map)
    assert <<len::32-big, json::binary>> = packet
    assert byte_size(json) == len
    assert {:ok, decoded, <<>>} = Codec.decode(packet)
    assert decoded["method"] == "test.ping"
    assert decoded["id"] == 1
  end

  test "incomplete frame" do
    assert :incomplete = Codec.decode(<<0, 0, 0, 10, "hi">>)
  end
end
