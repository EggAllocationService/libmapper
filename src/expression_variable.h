#ifndef __MPR_EXPRESSION_VARIABLE_H__
#define __MPR_EXPRESSION_VARIABLE_H__

#define MAX_HIST_SIZE 100
#define N_USER_VARS 16

/* Variables can have multiple dimensions, each of which may be indexed separately in an expression:
 *      input signals (in the case of VAR_X only)
 *      historic samples
 *      vector elements
 *      signal instances (not currently indexable)
 * Bitflags are used to keep track of which indices are provided, therefore the indices or
 * sub-expressions that compute the indices need to be presented in the same order. On the output
 * stack they are stored in the order: INST_IDX, VEC_IDX, HIST_IDX, SIG_IDX. On the operator stack
 * (during parsing) this order is reversed.
 * Input signal and vector indices can also be specified using an index stored directly in the
 * token. In this case the token flags are not set. */

#define VAR_SIG_IDX     0x0001
#define VAR_HIST_IDX    0x0002
#define VAR_VEC_IDX     0x0004
#define VAR_INST_IDX    0x0008

#define VAR_IDXS (VAR_HIST_IDX | VAR_VEC_IDX | VAR_SIG_IDX | VAR_INST_IDX)

uint8_t var_idx_nums[] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
#define NUM_VAR_IDXS(X) (var_idx_nums[X & VAR_IDXS])

typedef enum {
    VAR_UNKNOWN = -1,
    VAR_Y = N_USER_VARS,
    VAR_X_NEWEST,
    VAR_X,
    N_VARS
} expr_var_t;

#define VAR_ASSIGNED    0x0001
#define VAR_INSTANCED   0x0002
#define VAR_LEN_LOCKED  0x0004
#define VAR_SET_EXTERN  0x0008

typedef struct _var {
    char *name;
    mpr_type datatype;
    uint8_t vec_len;
    uint8_t flags;
} mpr_var_t, *mpr_var;

static int find_var_by_name(mpr_var_t *vars, int n_vars, const char *str, int len)
{
    /* check if variable name matches known variable */
    int i;
    for (i = 0; i < n_vars; i++) {
        if (strlen(vars[i].name) == len && strncmp(vars[i].name, str, len) == 0)
            return i;
    }
    return -1;
}

static const char* _get_var_str_and_len(const char* str, int last_char, int *len)
{
    int idx = last_char;
    char c = str[idx];
    while (idx >= 0 && c && (isalpha(c) || isdigit(c) || '_' == c)) {
        if (--idx >= 0)
            c = str[idx];
    }
    *len = last_char - idx;
    return str + idx + 1;
}

#endif /* __MPR_EXPRESSION_VARIABLE_H__ */
