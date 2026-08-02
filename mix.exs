defmodule DesktopWebview.MixProject do
  use Mix.Project

  @version "0.1.0"
  @url "https://github.com/elixir-desktop/webview"

  def project do
    [
      app: :desktop_webview,
      version: @version,
      elixir: "~> 1.15",
      elixirc_paths: elixirc_paths(Mix.env()),
      start_permanent: Mix.env() == :prod,
      deps: deps(),
      aliases: aliases(),
      description: description(),
      package: package(),
      source_url: @url,
      docs: [
        main: "readme",
        extras: [
          "README.md",
          "docs/protocol.md",
          "docs/packaging.md",
          "docs/desktop-integration.md",
          "docs/status/macos.md",
          "docs/status/windows.md",
          "docs/status/linux.md"
        ]
      ]
    ]
  end

  def application do
    [
      extra_applications: [:logger],
      mod: {DesktopWebview.Application, []}
    ]
  end

  defp elixirc_paths(:test), do: ["lib", "test/support"]
  defp elixirc_paths(_), do: ["lib"]

  defp deps do
    [
      {:desktop, path: desktop_dep_path(), override: true},
      {:jason, "~> 1.4"},
      {:ex_doc, ">= 0.0.0", only: :dev, runtime: false}
    ]
  end

  defp desktop_dep_path do
    cond do
      File.dir?(Path.expand("../desktop")) -> Path.expand("../desktop")
      File.dir?(Path.expand("desktop")) -> Path.expand("desktop")
      true -> Path.expand("../desktop")
    end
  end

  defp aliases do
    [
      "test.e2e": ["test --only e2e"],
      "test.unit": ["test --exclude e2e"]
    ]
  end

  defp description do
    "Native desktop webview host (WKWebView / WebKitGTK / WebView2) for elixir-desktop"
  end

  defp package do
    [
      name: "desktop_webview",
      licenses: ["MIT"],
      links: %{"GitHub" => @url},
      files: ~w(lib priv native/macos/README.md mix.exs README.md AGENTS.md LICENSE docs)
    ]
  end
end
