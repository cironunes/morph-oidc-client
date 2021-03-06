module OidcClient = {
  open Lwt.Syntax;

  let start = () => {
    let provider_uri =
      Uri.of_string("https://" ++ Sys.getenv("PROVIDER_HOST"));
    let _redirect_uri = Uri.of_string(Sys.getenv("OIDC_REDIRECT_URI"));

    let kv:
      module OidcClient.KeyValue.KV with
        type value = string and type store = Hashtbl.t(string, string) =
      (module OidcClient.KeyValue.MemoryKV);

    /*
     let+ microsoft_client =
       OidcClient.Microsoft.make(
         ~kv,
         ~store=Hashtbl.create(128),
         ~app_id="2824b599-24f1-4595-b63b-0cab4bb26c24",
         ~tenant_id="5c26a08a-b3ca-475d-abfc-a96df0a5593e",
         ~redirect_uri,
         ~secret=Sys.getenv_opt("OIDC_SECRET"),
       )
       |> Lwt_result.map_err(Piaf.Error.to_string);
       Hashtbl.add(oidc_clients_tbl, "microsoft", microsoft_client);
       */

    let+ certification_clients =
      Library.CertificationClients.get_clients(
        ~kv,
        ~make_store=() => Hashtbl.create(128),
        ~provider_uri,
      );

    let oidc_clients_tbl = Hashtbl.create(16);

    let () =
      List.iter(
        x => {
          switch (x) {
          | Ok((data, client)) =>
            Library.CertificationClients.(
              Hashtbl.add(oidc_clients_tbl, data.name, client)
            )
          | Error(e) => Logs.err(m => m("%s", Piaf.Error.to_string(e)))
          }
        },
        certification_clients,
      );
    Ok(oidc_clients_tbl);
  };

  let stop = oidc_clients_tbl => {
    Logs.info(m => m("Stopping OIDC Clients"));

    Hashtbl.to_seq(oidc_clients_tbl)
    |> Array.of_seq
    |> Array.map(((_, client): (string, OidcClient.t('a))) =>
         Piaf.Client.shutdown(client.http_client)
       )
    |> Array.to_list
    |> Lwt.join;
  };

  let component = Archi_lwt.Component.make(~start, ~stop);
};

module WebServer = {
  open Library;

  let port =
    try(Sys.getenv("PORT") |> int_of_string) {
    | _ => 4040
    };

  let server = Morph.Server.make(~port, ~address=Unix.inet_addr_any, ());

  let start = ((), context) => {
    Logs.info(m => m("Starting server on %n", port));

    let handler =
      Morph.Session.middleware(
        Context.middleware(
          ~context,
          Router.handler(
            Router.routes(~providers=Library.CertificationClients.datas),
          ),
        ),
      );

    server.start(handler) |> Lwt_result.ok;
  };

  let stop = _server => {
    Logs.info(m => m("Stopped Server")) |> Lwt.return;
  };

  let component =
    Archi_lwt.Component.using(
      ~start,
      ~stop,
      ~dependencies=[OidcClient.component],
    );
};

let system =
  Archi_lwt.System.make([
    ("oidc_client", OidcClient.component),
    ("web_server", WebServer.component),
  ]);

open Lwt.Infix;

let main = () => {
  Fmt_tty.setup_std_outputs();
  Logs.set_level(~all=true, Some(Logs.Info));
  Logs.set_reporter(Logs_fmt.reporter());
  let () = Mirage_crypto_rng_unix.initialize();

  Archi_lwt.System.start((), system)
  >|= (
    fun
    | Ok(system) => {
        Logs.info(m => m("Starting"));

        Sys.(
          set_signal(
            sigint,
            Signal_handle(
              _ => {
                Logs.err(m => m("SIGNINT received, tearing down.@."));
                Archi_lwt.System.stop(system) |> ignore;
              },
            ),
          )
        );
      }
    | Error(error) => {
        Logs.err(m => m("ERROR: %s@.", error));
        exit(1);
      }
  );
};

let () = Lwt_main.run(main());
