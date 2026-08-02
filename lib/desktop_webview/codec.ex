defmodule DesktopWebview.Codec do
  @moduledoc false

  @doc "Encode a JSON-RPC map to a length-prefixed packet."
  def encode(map) when is_map(map) do
    json = Jason.encode!(map)
    <<byte_size(json)::32-big, json::binary>>
  end

  @doc "Decode one frame from buffer. Returns `{message, rest}` or `:incomplete`."
  def decode(<<len::32-big, rest::binary>>) when byte_size(rest) >= len do
    <<json::binary-size(len), rest2::binary>> = rest
    {:ok, Jason.decode!(json), rest2}
  end

  def decode(_), do: :incomplete

  def request(id, method, params) do
    %{
      "jsonrpc" => "2.0",
      "id" => id,
      "method" => method,
      "params" => params || %{}
    }
  end

  def notification(method, params) do
    %{
      "jsonrpc" => "2.0",
      "method" => method,
      "params" => params || %{}
    }
  end

  def response(id, result) do
    %{"jsonrpc" => "2.0", "id" => id, "result" => result}
  end

  def error_response(id, code, message) do
    %{
      "jsonrpc" => "2.0",
      "id" => id,
      "error" => %{"code" => code, "message" => message}
    }
  end
end
