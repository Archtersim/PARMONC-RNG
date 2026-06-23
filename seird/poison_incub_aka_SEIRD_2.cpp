#define _CRT_SECURE_NO_WARNINGS

//#define DEBUG
//#define DEBUG_trajectory
//#define DEBUG_P
//#define DEBUG_SEEIRD_by_i

#include <iostream>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <list>
#include <iterator>
#include <vector>
#include <queue>
using namespace std;

#include <ctime>
 
#ifdef __unix
#define fopen_s(pFile,filename,mode) ((*(pFile))=fopen((filename),(mode)))==NULL
#endif

// for windows too
#define fopen_s(pFile,filename,mode) ((*(pFile))=fopen((filename),(mode)))==NULL

/*#define RAND RAND517()

//
unsigned long int ra1 = 1, ra2 = 0, ra3 = 0;
unsigned long int rb1 = 11973, rb2 = 2800, rb3 = 2842;
double rx1 = 1.0 / 4096.0, rx2 = 1.0 / 4096. / 16384.0, rx3 = 1.0 / 4096. /
16384.0 / 16384.0;

double RAND517(void)
{
    register unsigned long int rd1, rd2;

    rd1 = rb1 * ra1;
    rd2 = rb2 * ra1 + rb1 * ra2 + (rd1 >> 14);
    ra3 = (rb3 * ra1 + rb2 * ra2 + rb1 * ra3 + (rd2 >> 14)) & 4095;
    ra1 = rd1 & 16383;
    ra2 = rd2 & 16383;
    return ra3 * rx1 + ra2 * rx2 + ra1 * rx3;
}*/



#define RAND rnd128_()

//
// 128-bit pseudorandom number generator: returns alpha ~ U(0, 1)

double rnd128_()
{
    static int u[10] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    const int m[10] = { 1941, 1821, 3812, 1310, 68, 2906, 2335, 2609, 6859, 1999 };
    const double x[10] = {
        0.00000000000000000000000000000000000000293873587705571880,
        0.00000000000000000000000000000000002407412430484044800000,
        0.00000000000000000000000000000019721522630525295000000000,
        0.00000000000000000000000000161558713389263220000000000000,
        0.00000000000000000000001323488980084844300000000000000000,
        0.00000000000000000010842021724855044000000000000000000000,
        0.00000000000000088817841970012523000000000000000000000000,
        0.00000000000727595761418342590000000000000000000000000000,
        0.00000005960464477539062500000000000000000000000000000000,
        0.00048828125000000000000000000000000000000000000000000000 };
    int n, c0, c1, c2, c3, c4, c5, c6, c7, c8, c9;

    c0 = m[0] * u[0];
    c1 = m[0] * u[1] + m[1] * u[0];
    c2 = m[0] * u[2] + m[1] * u[1] + m[2] * u[0];
    c3 = m[0] * u[3] + m[1] * u[2] + m[2] * u[1] + m[3] * u[0];
    c4 = m[0] * u[4] + m[1] * u[3] + m[2] * u[2] + m[3] * u[1] + m[4] * u[0];
    c5 = m[0] * u[5] + m[1] * u[4] + m[2] * u[3] + m[3] * u[2] + m[4] * u[1] + m[5] * u[0];
    c6 = m[0] * u[6] + m[1] * u[5] + m[2] * u[4] + m[3] * u[3] + m[4] * u[2] + m[5] * u[1]
        + m[6] * u[0];
    c7 = m[0] * u[7] + m[1] * u[6] + m[2] * u[5] + m[3] * u[4] + m[4] * u[3] + m[5] * u[2]
        + m[6] * u[1] + m[7] * u[0];
    c8 = m[0] * u[8] + m[1] * u[7] + m[2] * u[6] + m[3] * u[5] + m[4] * u[4] + m[5] * u[3]
        + m[6] * u[2] + m[7] * u[1] + m[8] * u[0];
    c9 = m[0] * u[9] + m[1] * u[8] + m[2] * u[7] + m[3] * u[6] + m[4] * u[5] + m[5] * u[4]
        + m[6] * u[3] + m[7] * u[2] + m[8] * u[1] + m[9] * u[0];

    u[0] = c0 - ((c0 >> 13) << 13);
    n = c1 + (c0 >> 13);
    u[1] = n - ((n >> 13) << 13);
    n = c2 + (n >> 13);
    u[2] = n - ((n >> 13) << 13);
    n = c3 + (n >> 13);
    u[3] = n - ((n >> 13) << 13);
    n = c4 + (n >> 13);
    u[4] = n - ((n >> 13) << 13);
    n = c5 + (n >> 13);
    u[5] = n - ((n >> 13) << 13);
    n = c6 + (n >> 13);
    u[6] = n - ((n >> 13) << 13);
    n = c7 + (n >> 13);
    u[7] = n - ((n >> 13) << 13);
    n = c8 + (n >> 13);
    u[8] = n - ((n >> 13) << 13);
    n = c9 + (n >> 13);
    u[9] = n - ((n >> 11) << 11);

    return u[0] * x[0] + u[1] * x[1] + u[2] * x[2] + u[3] * x[3] + u[4] * x[4] + u[5] * x[5] + u[6] * x[6] + u[7] * x[7] + u[8] * x[8] + u[9] * x[9];
}



int INDEX_MIN(int dim_massif, double *massif)//Вычисление индекса минимального времени.
{

    int index_min;
   
    index_min = 0;
   
    for (int i = 1; i <= (dim_massif-1); i++)
    {

        if (massif[i] < massif[index_min])
        {

            index_min = i;

        }

    }

    return(index_min);
}

void print_SEIRD(int n, double *S, double *E_inc, double *E, double *I, double *R, double *D){
	FILE* pf;
    fopen_s(&pf, "SEEIRD.txt", "w");
    
	int width = 10;
	fprintf(pf, "%-*s %-*s %-*s %-*s %-*s %-*s %-*s\n", width, "day",  width, "S", width,"E_inc", width, "E", width, "I",width, "R", width, "D");
    for (int i = 0; i < n; i++){
        fprintf(pf, "%-*d %-*.0f %-*.0f %-*.0f %-*.0f %-*.0f %-*.0f\n", width,i, width,S[i], width,E_inc[i],width, E[i],width, I[i],width, R[i], width, D[i]);
    }
    fclose(pf);	
}


int main(void)
{
	unsigned int start_time = clock();

    double alfaE, alfaI, kappa, ro, beta, mu, p;
    double Bm, PSIm, TAUm, tm, round_tm, tinc, round_tinc, alfa, coef, N0, tm_ExitInc, dim_ExitInc;
    int i, j, k, n, dim_events, Nr, Tmod; 
	int index_tm_last, index_tm_next, index_event, index_min_of_times;
	int num_tm_ExitInc, num_tm_ExitE, dim_array_times, num_points;
    list<double> listExitInc; 
    list<double>::iterator itlistExitInc; 


    // SETENV ------------------------------------------------------------------------- //
	
	//Количество реализаций.
    Nr = 100;
    //Временной интервал моделирования.
    Tmod = 90;
	//Коэффициент, на который пропорционально увеличиваем начальные данные и делим результат.
    coef = 1;
	
    //Количество событий модели:
	// S     -> E_inc 
	// E_inc -> E with prob = (1-p) .OR. I with prob = (p)
	// E     -> R
	// E     -> I 
	// I     -> R
	// I     -> D 

    dim_events = 6;

    //Количество индивидуумов.
    N0 = 2798170 * coef;
	double S0     = 0  * coef;
	double E_inc0 = 0  * coef;
	double E0     = 99 * coef;
	double I0     = 0  * coef;
	double R0     = 24 * coef;
	double D0     = 0  * coef;
	S0            = N0 - E0 - R0;

    //Интенсивности переходов индивидуумов между группами.
    alfaE = 0.999;
    alfaI = 0.999;
    kappa = 0.042;
	//ro    = 0.952;
	//ro    = 0.900;
	//ro    = 0.932;
	ro    = 0.922;
    //ro    = 0.650;
    beta  = 0.999;
    mu    = 0.0188;

    //Инкубационный период.
     //tinc = 1 / kappa;
	tinc = 3.0;

    round_tinc = ceil(tinc);//ceil любое число округляет в большую сторону.

    //Доля симптомных I среди выявленных (все симптомные выявляются), (1-p) - доля бессимптомных среди выявленных.
    p = 0.51;

	//Вероятность перехода индивидуума из группы E_inc в группу E в момент окончания инкубационного периода.
	double p_E_inc_2_E = exp(-tinc*kappa);

	
#ifdef DEBUG
    cout << "round_tinc = " << round_tinc << endl; 
#endif

    //Минимум выбирается из времён:
	// 1 - время первого события в общем пуасоновском потоке
	// 2 - время первого выхода из инкубационного потока
    dim_array_times = 2;

    // SETENV ------------------------------------------------------------------------- //

    int error_mem_alloc = 100;
    //Массив, в котором хранятся эти времена.	
	double* array_times = (double*)calloc((dim_array_times), sizeof(double));
	if (array_times == NULL) exit (error_mem_alloc++);

    //Массив, в котором хранятся коэффициенты bm.
    double* bm = (double*)calloc( dim_events, sizeof(double));
	if (bm == NULL) exit (error_mem_alloc++);

    //Массив, в котором хранятся вероятности событий.
    double* P = (double*)calloc((dim_events), sizeof(double));
	if (P == NULL) exit (error_mem_alloc++);

    //Массив, в котором хранятся количества индивидуумов группы S в каждый момент времени.
    double* S = (double*)calloc((Tmod + 1), sizeof(double));
	if (S == NULL) exit (error_mem_alloc++);
	
	/*//Массив, в котором хранятся мат. ожид. количества индивидуумов группы S в каждый момент времени.
    double* E_S = (double*)calloc((Tmod + 1), sizeof(double));
	if (E_S == NULL) exit (error_mem_alloc++);
	*/
	
	//Массив, в котором хранятся количества индивидуумов группы E_inc в каждый момент времени.
    double* E_inc = (double*)calloc((Tmod + 1), sizeof(double));
	if (E_inc == NULL) exit (error_mem_alloc++);

    //Массив, в котором хранятся количества индивидуумов группы E в каждый момент времени.
    double* E = (double*)calloc((Tmod + 1), sizeof(double));
	if (E == NULL) exit (error_mem_alloc++);

    //Массив, в котором хранятся количества индивидуумов группы I в каждый момент времени.
    double* I = (double*)calloc((Tmod + 1), sizeof(double));
	if (I == NULL) exit (error_mem_alloc++);
	
	//Массив, в котором хранится прирост количества индивидуумов группы I за 1 день.
    double* I_day = (double*)calloc((Tmod + 2), sizeof(double));
	if (I_day == NULL) exit (error_mem_alloc++);

    //Массив, в котором хранятся количества индивидуумов группы R в каждый момент времени.
    double* R = (double*)calloc((Tmod + 1), sizeof(double));
	if (R == NULL) exit (error_mem_alloc++);

    //Массив, в котором хранятся количества индивидуумов группы D в каждый момент времени.
    double* D = (double*)calloc((Tmod + 1), sizeof(double));
	if (D == NULL) exit (error_mem_alloc++);

    //Массив, в котором хранятся количества выявленных заражений f в каждый момент времени.
    double* f = (double*)calloc((Tmod + 2), sizeof(double));
	if (f == NULL) exit (error_mem_alloc++);

    //Массив, в котором хранятся суммы количества выявленных заражений f по всем реализациям в каждый момент времени.
    double* sumf = (double*)calloc((Tmod + 2), sizeof(double));
	if (sumf == NULL) exit (error_mem_alloc++);

    //Массив, в котором хранятся суммы квадратов количества выявленных заражений f по всем реализациям в каждый момент времени.
    double* sumkvf = (double*)calloc((Tmod + 2), sizeof(double));
	if (sumkvf == NULL) exit (error_mem_alloc++);
	
    //Массив, в котором хранятся математические ожидания количества выявленных заражений f в каждый момент времени.
    double* Matogf = (double*)calloc((Tmod + 2), sizeof(double));
	if (Matogf == NULL) exit (error_mem_alloc++);

    //Массив, в котором хранятся дисперсии количества выявленных заражений f в каждый момент времени.
    double* Dispf = (double*)calloc((Tmod + 2), sizeof(double));
	if (Dispf == NULL) exit (error_mem_alloc++);

    //Массив, в котором хранятся статистические ошибки в каждый момент времени.
    double* stat_errorf = (double*)calloc((Tmod + 2), sizeof(double));
	if (stat_errorf == NULL) exit (error_mem_alloc++);

		
    for (n = 1; n <= Nr; n++)//Цикл для моделирования Nr реализаций.
    {
		cout << "n = " << n << endl; 
		
        //Обнуление массивов перед моделированием каждой n-ой реализации.
        for (i = 0; i <= (dim_events - 1); i++)
        {

            bm[i] = 0;
            P[i] = 0;

        }

        for (i = 0; i <= Tmod; i++)
        {

            S[i]     = 0;
			E_inc[i] = 0;
            E[i]     = 0;
            I[i]     = 0;
			I_day[i] = 0;
            R[i]     = 0;
            D[i]     = 0;

        }

        for (i = 0; i <= (Tmod + 1); i++)
        {

            f[i] = 0;

        }

        Bm            = 0;
        PSIm          = 0;
        TAUm          = 0;
        tm            = 0;
        round_tm      = 0;
        index_tm_last = 0;
        index_tm_next = 0;
        num_points    = 0;//Количество дней, который лежат между index_tm_next и index_tm_last.
        tm_ExitInc    = 0;
        dim_ExitInc   = 0;

        //Очистка списка перед моделированием каждой n-ой реализации.
        listExitInc.clear();

        //Начальные данные задачи.
        S[index_tm_last]     = S0;//N0-E0-R0=(2798170-99-24)*coef;
		E_inc[index_tm_last] = E_inc0;
        E[index_tm_last]     = E0;
        I[index_tm_last]     = I0;
		I_day[index_tm_last] = 0;
        R[index_tm_last]     = R0;
        D[index_tm_last]     = D0;
			

        while (tm <= Tmod)
        {
			int width = 10;
			#ifdef DEBUG_SEEIRD_by_i
			cout <<"index_tm_last - " << index_tm_last << " S:" << S[index_tm_last]  << " E_inc:" <<  E_inc[index_tm_last] << " E:" <<  E[index_tm_last] << " I:" <<  I[index_tm_last] <<" R:" <<  R[index_tm_last] << " D:" <<  D[index_tm_last] << endl;
			cout <<"index_tm_last - " << index_tm_last << " I_day:" << I_day[index_tm_last]<<endl;
			#endif

            dim_ExitInc = listExitInc.size();

            //Вычисление интенсивности времени пребывания, начиная с момента времени tm, до первого изменения численности индивидуумов в группах S(tm), E(tm), I(tm), R(tm), D(tm).
            bm[0] = ( alfaI * I[index_tm_last] + alfaE * (E[index_tm_last] + E_inc[index_tm_last]) ) * (S[index_tm_last] / N0);
            bm[1] = ro   * E[index_tm_last];
            bm[2] = beta * I[index_tm_last];
            bm[3] = mu   * I[index_tm_last];
			bm[4] = ro   * E_inc[index_tm_last];
			//bm[4] = 0;
			bm[5] = kappa* E[index_tm_last];
			
            Bm = 0;
            for (i = 0; i <= (dim_events - 1); i++)
            {

                Bm = Bm + bm[i];

            }

            //Вычисление времени пребывания, начиная с момента времени tm, 
			//до первого изменения численности индивидуумов в группах 
			//S(tm), E_inc(tm), E(tm), I(tm), R(tm), D(tm).  
            alfa = RAND;
            while (alfa == 0)
            alfa = RAND;
            PSIm = -log(alfa) / Bm;
            tm = tm + PSIm;
            array_times[0] = tm;

            if (listExitInc.empty())
            {
                //Если список пустой.
                tm_ExitInc = DBL_MAX;
            }
            else
            {
                //Если список не пустой.
                tm_ExitInc = listExitInc.front();
            }
            array_times[1] = tm_ExitInc;


            //Определение, какое из времён минимальное (смоделированное на n-ой итерации или из списка времён) и моделирование соответствующего события.
            index_min_of_times = INDEX_MIN(dim_array_times, array_times);

            tm = array_times[index_min_of_times];
			#ifdef DEBUG
			cout<<"Time:  "<< endl;
			cout<<"     t0 "<<array_times[0]<<"     t1 "<<array_times[1]<<endl;
			cout<<"     tm: " << tm <<"   index_min_of_times: "<< index_min_of_times<<endl;
			#endif
			
			if (tm > Tmod){
				//Количество точек, через которые перескочили.
				num_points = Tmod - index_tm_last;//Тут включается последняя точка Tmod.
				if (num_points > 0){
					for (i = 1; i <= num_points; i++){
                            S[index_tm_last + i]     = S[index_tm_last];
							E_inc[index_tm_last + i] = E_inc[index_tm_last];
                            E[index_tm_last + i]     = E[index_tm_last];
                            I[index_tm_last + i]     = I[index_tm_last];
							I_day[index_tm_last + i] = 0;
                            R[index_tm_last + i]     = R[index_tm_last];
                            D[index_tm_last + i]     = D[index_tm_last];
					}
				}
                break;
            }
	
			round_tm      = ceil(tm);//ceil любое число округляет в большую сторону.
			index_tm_next = (int)round_tm;//Преобразование из типа double в тип int.
			//Количество точек, через которые перескочили.
            num_points    = index_tm_next - index_tm_last - 1;
			if (num_points > 0){
				for (i = 1; i <= num_points; i++){
					S[index_tm_last + i]     = S[index_tm_last];
					E_inc[index_tm_last + i] = E_inc[index_tm_last];
                    E[index_tm_last + i]     = E[index_tm_last];
                    I[index_tm_last + i]     = I[index_tm_last];
					I_day[index_tm_last + i] = 0;
                    R[index_tm_last + i]     = R[index_tm_last];
                    D[index_tm_last + i]     = D[index_tm_last];
                }
			}
			
			
            if (index_min_of_times == 0)
            {
                //Вычисление вероятностей событий.
                for (i = 0; i <= (dim_events - 1); i++){
					P[i] = bm[i] / Bm;
				}
				
				#ifdef DEBUG_P
				cout<< "p ************* " << endl;
				cout<< "p0: "<< P[0] << endl;
				cout<< "p1: "<< P[1] << endl;
				cout<< "p2: "<< P[2] << endl;
				cout<< "p3: "<< P[3] << endl;
				cout<< "p4: "<< P[4] << endl;
				cout<< "p5: "<< P[5] << endl;
				#endif

                //Моделирование события в момент времени tm.
                index_event = 0;
                alfa = RAND;

                for (i = 0; i <= (dim_events - 1); i++)
                {
                    alfa = alfa - P[i];
                    if (alfa <= 0)
                    {
                        index_event = i;
                        break;
                    }
                }
				
                if (index_event == 0)
                {
                    S[index_tm_next]     = S[index_tm_last]     - 1;
                    E_inc[index_tm_next] = E_inc[index_tm_last] + 1;
					E[index_tm_next]     = E[index_tm_last];
                    I[index_tm_next]     = I[index_tm_last];
					I_day[index_tm_next] = I_day[index_tm_last];
                    R[index_tm_next]     = R[index_tm_last];
                    D[index_tm_next]     = D[index_tm_last];
                    // add incubation time exit to listExitInc  
                    tm_ExitInc = tm + round_tinc;
                    listExitInc.push_back(tm_ExitInc);
					#ifdef DEBUG_time
					cout<< "time exit: "<< tm_ExitInc << endl;
					cout<< "listExitInc.size: "<< listExitInc.size() << endl;
					cout<< "listExitInc.empty: "<< listExitInc.empty() << endl;
					//exit(0);
					#endif
                }

                if (index_event == 1)
                {
                    S[index_tm_next]     = S[index_tm_last];
					E_inc[index_tm_next] = E_inc[index_tm_last];
                    E[index_tm_next]     = E[index_tm_last] - 1;
                    I[index_tm_next]     = I[index_tm_last];
					I_day[index_tm_next] = I_day[index_tm_last];
                    R[index_tm_next]     = R[index_tm_last] + 1;
                    D[index_tm_next]     = D[index_tm_last];

                }

                if (index_event == 2)
                {
                    S[index_tm_next]     = S[index_tm_last];
					E_inc[index_tm_next] = E_inc[index_tm_last];
                    E[index_tm_next]     = E[index_tm_last];
                    I[index_tm_next]     = I[index_tm_last] - 1;
					I_day[index_tm_next] = I_day[index_tm_last];
                    R[index_tm_next]     = R[index_tm_last] + 1;
                    D[index_tm_next]     = D[index_tm_last];
                }

                if (index_event == 3)
                {
                    S[index_tm_next]     = S[index_tm_last];
					E_inc[index_tm_next] = E_inc[index_tm_last];
                    E[index_tm_next]     = E[index_tm_last];
                    I[index_tm_next]     = I[index_tm_last] - 1;
					I_day[index_tm_next] = I_day[index_tm_last];
                    R[index_tm_next]     = R[index_tm_last];
                    D[index_tm_next]     = D[index_tm_last] + 1;

                }
				
				if (index_event == 4)
                {	
                    S[index_tm_next]     = S[index_tm_last];
					E_inc[index_tm_next] = E_inc[index_tm_last] - 1;
                    E[index_tm_next]     = E[index_tm_last];
                    I[index_tm_next]     = I[index_tm_last];
					I_day[index_tm_next] = I_day[index_tm_last];
                    R[index_tm_next]     = R[index_tm_last]     + 1;
                    D[index_tm_next]     = D[index_tm_last];
					
					//remove random time from listExitInc
					int  size_ExitInc = listExitInc.size();
					alfa = RAND * size_ExitInc;
					j    = (int)alfa;
					if (j == size_ExitInc) j--;
					#ifdef DEBUG
					cout<< "index_event 4: " << endl;
					cout<< "listExitInc.size: "<< listExitInc.size() << endl;
					cout<< "listExitInc.empty: "<< listExitInc.empty() << endl;
					cout<< "listExitInc.front: "<< listExitInc.front() << endl;
					cout<< "j: "<< j << endl;
					#endif
					itlistExitInc = listExitInc.begin();
                    advance(itlistExitInc, j);
					if (!listExitInc.empty()){
						listExitInc.erase(itlistExitInc);
					}
					#ifdef DEBUG
					cout<< "index_event 4: " << endl;
					cout<< "2 listExitInc.size: "<< listExitInc.size() << endl;
					cout<< "2 listExitInc.empty: "<< listExitInc.empty() << endl;
					cout<< "2 j: "<< j << endl;
					cout<< "2 listExitInc.front: "<< listExitInc.front() << endl;
					#endif
                }
				if (index_event == 5)
                {

                    S[index_tm_next]     = S[index_tm_last];
					E_inc[index_tm_next] = E_inc[index_tm_last];
                    E[index_tm_next]     = E[index_tm_last] -1;
                    I[index_tm_next]     = I[index_tm_last] +1;
					I_day[index_tm_next] = I_day[index_tm_last]+1;
                    R[index_tm_next]     = R[index_tm_last];
                    D[index_tm_next]     = D[index_tm_last];
                }
            }

            if (index_min_of_times == 1){
			    #ifdef DEBUG
				cout<< "index_min_of_times = 1"<< endl;
				cout<< "tm = " << tm << endl;
				//exit(0);
				#endif
				
				S[index_tm_next]     = S[index_tm_last];
				E_inc[index_tm_next] = E_inc[index_tm_last] - 1;
				E[index_tm_next]     = E[index_tm_last];
				I[index_tm_next]     = I[index_tm_last];
				I_day[index_tm_next] = I_day[index_tm_last];
				R[index_tm_next]     = R[index_tm_last];
                D[index_tm_next]     = D[index_tm_last];
				
				#ifdef DEBUG
					cout<< "!listExitInc.empty " << !listExitInc.empty() << endl;
				#endif
				
				if (!listExitInc.empty()){
					itlistExitInc = listExitInc.begin();
					listExitInc.erase(itlistExitInc);
				}
				
				alfa = RAND;
                if (alfa <= p_E_inc_2_E){
					E[index_tm_next]++;
				} else{
					I[index_tm_next]++;
					I_day[index_tm_next]++;
				}
			}
			
			//Проверка условия, когда E_inc= 0, E=0 и I=0 одновременно и 
			//выход из цикла моделирования одной реализации, 
			//переход на моделирование следующей реализации.
			if ( (E_inc[index_tm_next] == 0) && (E[index_tm_next] == 0) && (I[index_tm_next] == 0)){
				//printf ("%d %d\n", index_tm_next, n);//Выводит с какого дня выход на ноль и на какой реализации. 
				for (i = index_tm_next + 1; i <= Tmod; i++){
                    S[i]     = S[index_tm_next];
					E_inc[i] = E_inc[index_tm_next];
                    E[i]     = E[index_tm_next];
                    I[i]     = I[index_tm_next];
					I_day[i] = 0;
                    R[i]     = R[index_tm_next];
                    D[i]     = D[index_tm_next];
				}
				break;
			}
			index_tm_last = index_tm_next;
        }
		#ifdef DEBUG_trajectory
    		print_SEIRD(Tmod + 1, S, E_inc, E, I, R, D);
    		exit(0);
		#endif
		for (i = 1; i <= (Tmod); i++){
			f[i] = (I_day[i]-I_day[i-1])/ p;
        }
		f[Tmod + 1] = (E[Tmod]+E_inc[Tmod])*kappa/p;
		
		for (i = 1; i <= (Tmod + 1); i++){
            sumf[i] = sumf[i] + f[i];
            sumkvf[i] = sumkvf[i] + f[i] * f[i];
        }

    }

    for (i = 1; i <= (Tmod + 1); i++)
    {

        Matogf[i] = (sumf[i] / Nr);
        Dispf[i] = ((Nr) / (Nr - 1)) * ((sumkvf[i] / Nr) - (Matogf[i] * Matogf[i]));
        stat_errorf[i] = (sqrt(Dispf[i] / Nr));

    }

    //Вывод в файл значений математического ожидания количества выявленных заражений f и статистической ошибки в каждый момент времени.
    FILE* date_Matogf;
    fopen_s(&date_Matogf, "Matogf.txt", "w");

    FILE* date_stat_errorf;
    fopen_s(&date_stat_errorf, "stat_errorf.txt", "w");
	
	//FILE* date_SEIRD;
    //fopen_s(&date_SEIRD, "SEIRD.txt", "w");

    //Matogf[1] = (kappa * E[0] * coef) / (0.51 * coef);
    //stat_errorf[1] = 0;
	fprintf(date_Matogf, "day identified lval uval\n");
	fprintf(date_stat_errorf, "day error\n");
	//fprintf(date_SEIRD, "day S E_inc E I R D \n");
	
    for (i = 1; i <= (Tmod + 1); i++)
    {

        fprintf(date_Matogf, "%d %f %f %f\n", i, Matogf[i] / coef, ((Matogf[i] / coef) - (sqrt(Dispf[i]) / coef)), ((Matogf[i] / coef) + (sqrt(Dispf[i]) / coef)));
        fprintf(date_stat_errorf, "%d %f\n", i, stat_errorf[i] / coef);

    }
    fclose(date_Matogf);
    fclose(date_stat_errorf);
	//fclose(date_SEIRD);

    //Очистка памяти.
    free(array_times);
    free(bm);
    free(P);
    free(S);
	free(E_inc);
    free(E);
    free(I);
	free(I_day);
    free(R);
    free(D);
    free(f);
    free(sumf);
    free(sumkvf);
    free(Matogf);
    free(Dispf);
    free(stat_errorf);
    listExitInc.clear();
	
	unsigned int time_elapsed = (clock() - start_time);
	FILE* ftime;
    fopen_s(&ftime, "time_elapsed.txt", "w");
	fprintf(ftime, "Time elapsed: %f seconds", ((float)time_elapsed)/CLOCKS_PER_SEC);
	fclose(ftime);
	
    return(0);
}
