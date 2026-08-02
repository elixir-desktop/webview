defmodule DesktopWebview do
  @moduledoc """
  Native desktop webview host client for elixir-desktop.

  Configure:

      config :desktop, :backend, DesktopWebview.Backend
      config :desktop, :menu_adapter, DesktopWebview.Menu.Adapter
  """

  @doc "Protocol version spoken by this client."
  def protocol_version, do: 1
end
