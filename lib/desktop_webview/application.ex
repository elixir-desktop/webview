defmodule DesktopWebview.Application do
  @moduledoc false
  use Application

  @impl true
  def start(_type, _args) do
    # Transport is started on demand via DesktopWebview.Transport.ensure_started/0
    # (and by E2E tests) so it is not permanently supervised here.
    Supervisor.start_link([], strategy: :one_for_one, name: DesktopWebview.Supervisor)
  end
end
