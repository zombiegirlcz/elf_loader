int mod_global = 100;
static int mod_counter = 0;

int mod_add(int a, int b) {
    mod_counter++;
    return a + b + mod_global;
}

int mod_count(void) {
    return mod_counter;
}