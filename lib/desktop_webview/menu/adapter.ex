defmodule DesktopWebview.Menu.Adapter do
  @moduledoc """
  `Desktop.Menu.Adapter` that snapshots menu DOM to the native host over JSON-RPC.
  """

  defstruct [:menu_pid, :menubar_opts, :menubar, :taskbar_icon, :dom]

  alias DesktopWebview.Transport

  def new(opts) do
    %__MODULE__{
      menu_pid: Keyword.get(opts, :menu_pid),
      menubar_opts: Keyword.get(opts, :wx),
      menubar: nil,
      taskbar_icon: nil,
      dom: nil
    }
  end

  def create(adapter, dom) do
    create_menubar(adapter, adapter.menubar_opts, dom)
  end

  def update_dom(adapter, dom), do: create_menu(adapter, dom)
  def popup_menu(adapter), do: adapter
  def recreate_menu(adapter, dom), do: create_menu(adapter, dom)
  def menubar(%{menubar: m}), do: m
  def get_icon(%{taskbar_icon: t}), do: t

  def set_icon(adapter = %{taskbar_icon: nil}, nil), do: {:ok, adapter}

  def set_icon(adapter = %{taskbar_icon: tray_id}, nil) when is_binary(tray_id) do
    _ = Transport.call("tray.destroy", %{"tray_id" => tray_id})
    {:ok, %{adapter | taskbar_icon: nil}}
  end

  def set_icon(adapter = %{taskbar_icon: nil}, _icon), do: {:ok, adapter}

  def set_icon(adapter = %{taskbar_icon: tray_id}, icon) when is_binary(tray_id) do
    id = icon_id(icon)
    _ = Transport.call("tray.set_icon", %{"tray_id" => tray_id, "icon_id" => id})
    {:ok, adapter}
  end

  def handle_info(_event, adapter), do: {:noreply, adapter}

  defp create_menubar(adapter, {:taskbar, icon}, dom) do
    adapter = create_menu(adapter, dom)
    menu_id = menu_id(adapter.menubar)

    {:ok, %{"tray_id" => tray_id}} =
      case Transport.call("tray.create", %{
             "icon_id" => icon_id(icon),
             "menu_id" => menu_id
           }) do
        {:ok, res} -> {:ok, res}
        _ -> {:ok, %{"tray_id" => nil}}
      end

    %{adapter | taskbar_icon: tray_id}
  end

  defp create_menubar(adapter, _ref, dom) do
    create_menu(adapter, normalize_dom(dom))
  end

  defp create_menu(adapter, dom) do
    json = dom_to_json(dom)

    result =
      case adapter.menubar do
        {:menu, id} when is_binary(id) ->
          Transport.call("menu.update", %{"menu_id" => id, "dom" => json})
          adapter.menubar

        _ ->
          case Transport.call("menu.create", %{"kind" => "menubar", "dom" => json}) do
            {:ok, %{"menu_id" => id}} -> {:menu, id}
            _ -> {:menu, nil}
          end
      end

    %{adapter | menubar: result, dom: dom}
  end

  defp normalize_dom(dom), do: dom

  def dom_to_json({:menubar, attrs, children}) do
    %{
      "tag" => "menubar",
      "attrs" => attrs_to_map(attrs),
      "children" => Enum.map(List.wrap(children), &dom_to_json/1)
    }
  end

  def dom_to_json({:menu, attrs, children}) do
    %{
      "tag" => "menu",
      "attrs" => attrs_to_map(attrs),
      "children" => Enum.map(List.wrap(children), &dom_to_json/1)
    }
  end

  def dom_to_json({:item, attrs, children}) do
    %{
      "tag" => "item",
      "attrs" => attrs_to_map(attrs),
      "children" => Enum.map(List.wrap(children), &child_text/1)
    }
  end

  def dom_to_json({:hr, attrs, _}) do
    %{"tag" => "hr", "attrs" => attrs_to_map(attrs), "children" => []}
  end

  def dom_to_json(list) when is_list(list), do: Enum.map(list, &dom_to_json/1)
  def dom_to_json(other), do: %{"tag" => "unknown", "attrs" => %{}, "children" => [to_string(other)]}

  defp child_text(t) when is_binary(t), do: t
  defp child_text(t), do: to_string(t)

  defp attrs_to_map(attrs) when is_list(attrs) do
    Map.new(attrs, fn
      {k, v} -> {to_string(k), to_string(v)}
      other -> {"value", to_string(other)}
    end)
  end

  defp attrs_to_map(attrs) when is_map(attrs), do: Map.new(attrs, fn {k, v} -> {to_string(k), to_string(v)} end)
  defp attrs_to_map(_), do: %{}

  defp icon_id({:icon, id}), do: id
  defp icon_id({:image, id}), do: id
  defp icon_id(id) when is_binary(id), do: id
  defp icon_id(_), do: nil

  defp menu_id({:menu, id}), do: id
  defp menu_id(id) when is_binary(id), do: id
  defp menu_id(_), do: nil
end

defimpl Desktop.Menu.Adapter, for: DesktopWebview.Menu.Adapter do
  def create(adapter, dom), do: DesktopWebview.Menu.Adapter.create(adapter, dom)
  def update_dom(adapter, dom), do: DesktopWebview.Menu.Adapter.update_dom(adapter, dom)
  def popup_menu(adapter), do: DesktopWebview.Menu.Adapter.popup_menu(adapter)
  def recreate_menu(adapter, dom), do: DesktopWebview.Menu.Adapter.recreate_menu(adapter, dom)
  def menubar(adapter), do: DesktopWebview.Menu.Adapter.menubar(adapter)
  def get_icon(adapter), do: DesktopWebview.Menu.Adapter.get_icon(adapter)
  def set_icon(adapter, icon), do: DesktopWebview.Menu.Adapter.set_icon(adapter, icon)
end
