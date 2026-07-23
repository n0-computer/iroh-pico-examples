fn main() {
    embuild::build::LinkArgs::output_propagated("PICO_STD")
        .expect("pico-std linker arguments were not propagated");
}
