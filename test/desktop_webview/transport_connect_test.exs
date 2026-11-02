defmodule DesktopWebview.TransportConnectTest do
  use ExUnit.Case, async: false

  alias DesktopWebview.{Codec, Transport}

  setup do
    if pid = Process.whereis(Transport) do
      Process.exit(pid, :kill)
      Process.sleep(20)
    end

    Transport.ensure_started()
    Transport.subscribe(self())
    :ok
  end

  test "connect drops notifications written before the initialize result" do
    parent = self()
    {:ok, listen} = :gen_tcp.listen(0, [:binary, packet: 4, active: false, ip: {127, 0, 0, 1}])
    {:ok, port} = :inet.port(listen)
    on_exit(fn -> :gen_tcp.close(listen) end)

    server =
      spawn(fn ->
        {:ok, sock} = :gen_tcp.accept(listen)
        # Same shape as flush_queued/0 running inside accept_client/0.
        early =
          Codec.notification("event.system.open_url", %{"url" => "ddrive://queued-while-down"})

        :ok = :gen_tcp.send(sock, Jason.encode!(early))
        {:ok, raw} = :gen_tcp.recv(sock, 0, 5_000)
        req = Jason.decode!(raw)

        :ok =
          :gen_tcp.send(
            sock,
            Jason.encode!(Codec.response(req["id"], %{"protocol_version" => 1}))
          )

        later =
          Codec.notification("event.system.open_url", %{"url" => "ddrive://after-initialize"})

        :ok = :gen_tcp.send(sock, Jason.encode!(later))
        send(parent, :server_done)

        receive do
          :stop -> :ok
        end
      end)

    on_exit(fn -> send(server, :stop) end)

    assert {:ok, %{"protocol_version" => 1}} = Transport.connect("127.0.0.1", port)
    assert_receive :server_done, 1_000

    assert_receive {:edw_event, "event.system.open_url", %{"url" => "ddrive://after-initialize"}},
                   1_000

    refute_receive {:edw_event, "event.system.open_url",
                    %{"url" => "ddrive://queued-while-down"}},
                   200
  end
end
