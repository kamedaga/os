extern int lpr_needed_value(void);

int main(void)
{
    return lpr_needed_value() == 42 ? 0 : 1;
}
