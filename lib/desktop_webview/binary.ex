defmodule DesktopWebview.Binary do
  @moduledoc false

  @doc "Resolve path to the native DesktopWebView binary."
  def path do
    cond do
      path = System.get_env("DESKTOP_WEBVIEW_BINARY") -> path
      path = Application.get_env(:desktop_webview, :binary) -> path
      true -> default_path()
    end
  end

  def default_path do
    case :os.type() do
      {:unix, :darwin} ->
        Application.app_dir(:desktop_webview, ["priv", "native", "macos", "DesktopWebView"])

      {:win32, _} ->
        Application.app_dir(:desktop_webview, ["priv", "native", "windows", "DesktopWebView.exe"])

      {:unix, _} ->
        Application.app_dir(:desktop_webview, ["priv", "native", "linux", "DesktopWebView"])
    end
  end

  def available? do
    p = path()
    is_binary(p) and File.regular?(p)
  end
end
