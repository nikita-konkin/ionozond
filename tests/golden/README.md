# Golden files

Produced by the **original** dsChirp binary running headless in the dev
container, driven through Parameters -> OK -> Start with `drive_oracle.sh`:

    docker run --rm -v N:\ds_shirp_revers_eng:/work -v F:\MyData\ND\lfs:/data:ro \
        dschirp-dev bash /work/ionozond/tests/drive_oracle.sh \
        1493,131:params 1132,722:ok 1425,131:start

- `config.ini` / `schedule.ini` — the settings as the original left them, i.e.
  the exact INPUT that produced the output below.
- `chirp_config.py` — the exact OUTPUT of frmMain::CreateConfigFile().

`test_configwriter` regenerates the output from those inputs and requires a
byte-identical match. Do not hand-edit these files.
