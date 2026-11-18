defmodule DesktopWebview.AppUserModelId do
  @moduledoc false

  # Prebuilt by scripts/build_windows.ps1 → priv/native/windows/aumid_nif.dll
  # (not compiled via mix). Missing DLL keeps the Elixir stub (:ok no-op).

  @on_load :load_nif

  def load_nif do
    path =
      [:code.priv_dir(:desktop_webview), "native", "windows", "aumid_nif"]
      |> Path.join()
      |> String.to_charlist()

    case :erlang.load_nif(path, 0) do
      :ok -> :ok
      {:error, _} -> :ok
    end
  end

  # Host passes EDW_APP_USER_MODEL_ID so BEAM shares the host AppUserModelID.
  def apply_from_env do
    case System.get_env("EDW_APP_USER_MODEL_ID") do
      id when is_binary(id) and id != "" -> set(id)
      _ -> :ok
    end
  end

  def set(id) when is_binary(id), do: set_nif(id)

  def set_nif(_id), do: :ok
end
