defmodule DesktopWebview.MenuDomTest do
  use ExUnit.Case, async: true

  alias DesktopWebview.Menu.Adapter

  test "dom_to_json encodes menubar tree" do
    dom =
      {:menubar, [],
       [
         {:menu, [label: "File"],
          [
            {:item, [onclick: "quit"], ["Quit"]},
            {:hr, [], []}
          ]}
       ]}

    json = Adapter.dom_to_json(dom)
    assert json["tag"] == "menubar"
    assert hd(json["children"])["tag"] == "menu"
    assert hd(hd(json["children"])["children"])["attrs"]["onclick"] == "quit"
  end
end
