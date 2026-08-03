defmodule DesktopWebview.Dialog do
  @moduledoc """
  Native file / directory / text prompt dialogs via the DesktopWebView host.
  """

  alias DesktopWebview.Transport

  @timeout 600_000

  def choose_file(opts \\ []) do
    params =
      %{}
      |> maybe_put("title", opts[:title])
      |> maybe_put("default_path", opts[:default_path])

    case Transport.call("dialog.choose_file", params, @timeout) do
      {:ok, %{"path" => path}} when is_binary(path) -> path
      {:ok, nil} -> nil
      {:ok, _} -> nil
      {:error, reason} -> {:error, reason}
    end
  end

  def choose_directory(opts \\ []) do
    params =
      %{}
      |> maybe_put("title", opts[:title])
      |> maybe_put("default_path", opts[:default_path])

    case Transport.call("dialog.choose_directory", params, @timeout) do
      {:ok, %{"path" => path}} when is_binary(path) -> path
      {:ok, nil} -> nil
      {:ok, _} -> nil
      {:error, reason} -> {:error, reason}
    end
  end

  def prompt(title, message, default \\ "") do
    params = %{
      "title" => to_string(title),
      "message" => to_string(message),
      "default_value" => to_string(default)
    }

    case Transport.call("dialog.prompt", params, @timeout) do
      {:ok, %{"value" => value}} when is_binary(value) -> value
      {:ok, nil} -> nil
      {:ok, _} -> nil
      {:error, reason} -> {:error, reason}
    end
  end

  defp maybe_put(map, _key, nil), do: map
  defp maybe_put(map, key, value), do: Map.put(map, key, to_string(value))
end
