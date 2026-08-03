defmodule DesktopWebview.DialogTest do
  use ExUnit.Case, async: true

  test "module exports choose_file, choose_directory, prompt" do
    {:module, _} = Code.ensure_loaded(DesktopWebview.Dialog)
    assert function_exported?(DesktopWebview.Dialog, :choose_file, 1)
    assert function_exported?(DesktopWebview.Dialog, :choose_directory, 1)
    assert function_exported?(DesktopWebview.Dialog, :prompt, 2)
    assert function_exported?(DesktopWebview.Dialog, :prompt, 3)
  end
end
