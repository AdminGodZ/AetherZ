//! Trainer for AetherZ's NNUE: bullet's `examples/simple.rs` with a
//! configurable hidden size (256 by default) and command-line options.
//!
//! The architecture and quantisation must match `src/engine/nnue/network.hpp`:
//! inputs `Chess768`, `(768 -> N)x2 -> 1`, SCReLU, side to move's accumulator
//! first, `QA = 255`, `QB = 64`, `SCALE = 400`. Each checkpoint directory
//! `<output-dir>/<net-id>-<superbatch>/` contains `quantised.bin`, which the
//! engine loads with `setoption name EvalFile value <path>`.

use std::{env, process};

use bullet::{
    game::inputs::Chess768,
    nn::optimiser::AdamW,
    trainer::{
        save::SavedFormat,
        schedule::{TrainingSchedule, TrainingSteps, lr, wdl},
        settings::LocalSettings,
    },
    value::{ValueTrainerBuilder, loader},
};

const SCALE: i32 = 400;
const QA: i16 = 255;
const QB: i16 = 64;

const USAGE: &str = "\
Usage: aetherz-trainer [OPTIONS] <DATA.bf>...

Trains AetherZ's (768 -> N)x2 -> 1 network on BulletFormat data
(convert `aetherz datagen` output with `bullet-utils convert`, then shuffle).

Options (defaults are those of bullet's `simple` example):
  --data <FILE>                  Training data (repeatable; positional arguments work too)
  --output-dir <DIR>             Checkpoint directory                    [checkpoints]
  --net-id <ID>                  Network name; checkpoints are <ID>-<N>  [simple]
  --hidden-size <N>              Hidden layer size; the engine must be
                                 built for it (CMake AETHERZ_NNUE_HIDDEN) [256]
  --superbatches <N>             Last superbatch                         [40]
  --start-superbatch <N>         First superbatch                        [1]
  --start-lr <LR>                Initial learning rate                   [0.001]
  --final-lr <LR>                Cosine-decay to this LR by the last superbatch.
                                 Without it the LR drops 10x every --lr-step
                                 superbatches, as in `simple`.
  --lr-step <N>                  StepLR step                             [18]
  --wdl <P>                      Game-result share of the target         [0.75]
  --batch-size <N>               Positions per batch                     [16384]
  --batches-per-superbatch <N>   Batches per superbatch                  [6104]
  --save-rate <N>                Save every N superbatches (and the last) [10]
  --threads <N>                  Data-loading threads                    [4]
  -h, --help                     Show this help
";

struct Args {
    data: Vec<String>,
    output_dir: String,
    net_id: String,
    hidden_size: usize,
    superbatches: usize,
    start_superbatch: usize,
    start_lr: f32,
    final_lr: Option<f32>,
    lr_step: usize,
    wdl: f32,
    batch_size: usize,
    batches_per_superbatch: usize,
    save_rate: usize,
    threads: usize,
}

fn fail(msg: &str) -> ! {
    eprintln!("error: {msg}\n\n{USAGE}");
    process::exit(2);
}

fn parse<T: std::str::FromStr>(flag: &str, value: Option<String>) -> T {
    let value = value.unwrap_or_else(|| fail(&format!("{flag} needs a value")));
    value.parse().unwrap_or_else(|_| fail(&format!("invalid value for {flag}: {value}")))
}

fn parse_args() -> Args {
    let mut args = Args {
        data: Vec::new(),
        output_dir: "checkpoints".to_string(),
        net_id: "simple".to_string(),
        hidden_size: 256,
        superbatches: 40,
        start_superbatch: 1,
        start_lr: 0.001,
        final_lr: None,
        lr_step: 18,
        wdl: 0.75,
        batch_size: 16_384,
        batches_per_superbatch: 6104,
        save_rate: 10,
        threads: 4,
    };

    let mut it = env::args().skip(1);
    while let Some(arg) = it.next() {
        // Accept both `--flag value` and `--flag=value`.
        let (flag, inline) = match arg.split_once('=') {
            Some((f, v)) if f.starts_with("--") => (f.to_string(), Some(v.to_string())),
            _ => (arg.clone(), None),
        };
        let mut value = || inline.clone().or_else(|| it.next());
        match flag.as_str() {
            "-h" | "--help" => {
                print!("{USAGE}");
                process::exit(0);
            }
            "--data" => args.data.push(parse(&flag, value())),
            "--output-dir" => args.output_dir = parse(&flag, value()),
            "--net-id" => args.net_id = parse(&flag, value()),
            "--hidden-size" => args.hidden_size = parse(&flag, value()),
            "--superbatches" => args.superbatches = parse(&flag, value()),
            "--start-superbatch" => args.start_superbatch = parse(&flag, value()),
            "--start-lr" => args.start_lr = parse(&flag, value()),
            "--final-lr" => args.final_lr = Some(parse(&flag, value())),
            "--lr-step" => args.lr_step = parse(&flag, value()),
            "--wdl" => args.wdl = parse(&flag, value()),
            "--batch-size" => args.batch_size = parse(&flag, value()),
            "--batches-per-superbatch" => args.batches_per_superbatch = parse(&flag, value()),
            "--save-rate" => args.save_rate = parse(&flag, value()),
            "--threads" => args.threads = parse(&flag, value()),
            f if f.starts_with('-') => fail(&format!("unknown option {f}")),
            _ => args.data.push(arg),
        }
    }

    if args.data.is_empty() {
        fail("no training data given");
    }
    if args.start_superbatch == 0 || args.superbatches < args.start_superbatch {
        fail("need 1 <= --start-superbatch <= --superbatches");
    }
    if args.save_rate == 0 || args.lr_step == 0 || args.batch_size == 0 || args.batches_per_superbatch == 0 {
        fail("--save-rate, --lr-step, --batch-size and --batches-per-superbatch must be non-zero");
    }
    if args.hidden_size == 0 || args.hidden_size % 16 != 0 {
        fail("--hidden-size must be a positive multiple of 16");
    }
    if !(0.0..=1.0).contains(&args.wdl) {
        fail("--wdl must be in [0, 1]");
    }
    args
}

fn main() {
    let args = parse_args();
    let hidden_size = args.hidden_size;

    let mut trainer = ValueTrainerBuilder::default()
        // makes `ntm_inputs` available below
        .dual_perspective()
        // the default AdamW params include clipping to range [-1.98, 1.98]
        .optimiser(AdamW)
        .inputs(Chess768)
        // the layout `src/engine/nnue/network.hpp` reads
        .save_format(&[
            SavedFormat::id("l0w").round().quantise::<i16>(QA),
            SavedFormat::id("l0b").round().quantise::<i16>(QA),
            SavedFormat::id("l1w").round().quantise::<i16>(QB),
            SavedFormat::id("l1b").round().quantise::<i16>(QA * QB),
        ])
        // target = wdl * game_result + (1 - wdl) * sigmoid(score / SCALE)
        .loss_fn(|output, target| output.sigmoid().squared_error(target))
        .build(|builder, stm_inputs, ntm_inputs| {
            let l0 = builder.new_affine("l0", 768, hidden_size);
            let l1 = builder.new_affine("l1", 2 * hidden_size, 1);

            // side to move's accumulator first, as the engine expects
            let stm_hidden = l0.forward(stm_inputs).screlu();
            let ntm_hidden = l0.forward(ntm_inputs).screlu();
            l1.forward(stm_hidden.concat(ntm_hidden))
        });

    let steps = TrainingSteps {
        batch_size: args.batch_size,
        batches_per_superbatch: args.batches_per_superbatch,
        start_superbatch: args.start_superbatch,
        end_superbatch: args.superbatches,
    };
    let wdl_scheduler = wdl::ConstantWDL { value: args.wdl };

    let settings = LocalSettings {
        threads: args.threads,
        test_set: None,
        output_directory: &args.output_dir,
        batch_queue_size: 64,
    };

    let files: Vec<&str> = args.data.iter().map(String::as_str).collect();
    let data_loader = loader::DirectSequentialDataLoader::new(&files);

    // The LR scheduler is a type parameter of `TrainingSchedule`, hence two arms.
    match args.final_lr {
        Some(final_lr) => {
            let schedule = TrainingSchedule {
                net_id: args.net_id.clone(),
                eval_scale: SCALE as f32,
                steps,
                wdl_scheduler,
                lr_scheduler: lr::CosineDecayLR {
                    initial_lr: args.start_lr,
                    final_lr,
                    final_superbatch: args.superbatches,
                },
                save_rate: args.save_rate,
            };
            trainer.run(&schedule, &settings, &data_loader);
        }
        None => {
            let schedule = TrainingSchedule {
                net_id: args.net_id.clone(),
                eval_scale: SCALE as f32,
                steps,
                wdl_scheduler,
                lr_scheduler: lr::StepLR { start: args.start_lr, gamma: 0.1, step: args.lr_step },
                save_rate: args.save_rate,
            };
            trainer.run(&schedule, &settings, &data_loader);
        }
    }
}
