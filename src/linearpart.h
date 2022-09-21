/*  Taudem parallel linear partition classes

  David Tarboton, Kim Schreuders, Dan Watson
  Utah State University  
  May 23, 2010
  
*/

/*  Copyright (C) 2010  David Tarboton, Utah State University

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License 
version 2, 1991 as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

A copy of the full GNU General Public License is included in file 
gpl.html. This is also available at:
http://www.gnu.org/copyleft/gpl.html
or from:
The Free Software Foundation, Inc., 59 Temple Place - Suite 330, 
Boston, MA  02111-1307, USA.

If you wish to use or incorporate this program (or parts of it) into 
other software that does not meet the GNU General Public License 
conditions contact the author to request permission.
David G. Tarboton  
Utah State University 
8200 Old Main Hill 
Logan, UT 84322-8200 
USA 
http://www.engineering.usu.edu/dtarb/ 
email:  dtarb@usu.edu 
*/

//  This software is distributed from http://hydrology.usu.edu/taudem/

#include "mpi.h"
#include "partition.h"

#include <queue>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <exception>
#ifndef LINEARPART_H
#define LINEARPART_H
using namespace std;

template <class datatype>
class linearpart : public tdpartition {
	protected:
		// Member data inherited from partition
		//long totalx, totaly;
		//long nx, ny;
		//double dx, dy;
		long starty;
		int rank, size;
		MPI_Datatype MPI_type;
		datatype noData;
		datatype *gridData;
		datatype *topBorder;
		datatype *bottomBorder;
		datatype *passBordersBuffer;

	public:
		linearpart():tdpartition(){}
		~linearpart();

		void init(long totalx, long totaly, double dx_in, double dy_in, MPI_Datatype MPIt, datatype nd);
		bool isInPartition(int x, int y);
		bool hasAccess(int x, int y);

		void share();
		void passBorders();
		void addBorders();
		void clearBorders();
		int ringTerm(int isFinished);
		
		bool globalToLocal(int globalX, int globalY, int &localX, int &localY);
		void localToGlobal(int localX, int localY, int &globalX, int &globalY);

		int getGridXY( int x,int y, int *i, int *j);
		void transferPack( int *, int *, int *, int*);

		// Member functions inherited from partition
		//int getnx() {return nx;}
		//int getny() {return ny;} 
		//int gettotalx(){return totalx;}
		//int gettotaly(){return totaly;}
		void* getGridPointer(){return gridData;}
		bool isNodata(long x, long y);
		void setToNodata(long x, long y);
		datatype getData(long x, long y, datatype &val);
		void setData(long x, long y, datatype val);
		void savedxdyc(tiffIO &obj);
		void getdxdyc(long iny, double &val_dxc,double &val_dyc);
		void addToData(long x, long y, datatype val);
				
		//void areaD(queue<node> *que);

		//inherited from partition
		//int *before1;
		//int *before2;
		//int *after1;
		//int *after2;

};


//Destructor.  Just frees up memory.
template <class datatype>
linearpart<datatype>::~linearpart(){
	delete [] gridData;
	delete [] bottomBorder;
	delete [] topBorder;
	delete [] passBordersBuffer;
}

//Init routine.  Takes the total number of rows and columns in the ENTIRE grid to be partitioned,
//dx and dy for the grid, MPI datatype (should match the template declaration), and noData value.
template <class datatype>
void linearpart<datatype>::init(long totalx, long totaly, double dx_in, double dy_in, MPI_Datatype MPIt, datatype nd){
	MPI_Comm_rank(MCW,&rank);
	MPI_Comm_size(MCW,&size);

	//Store all the initialization variables in their appropriate places
	this->totalx = totalx;
	this->totaly = totaly;
	nx = totalx;
	starty = (rank * (int64_t)totaly) / size;
	ny = ((rank+1) * (int64_t)totaly) / size - starty;
	if ( ny < 2 ) {
		fprintf(stdout,"Too many MPI ranks.  Rank %d only has %ld rows.\n", rank, ny);
		fflush(stdout);
		MPI_Abort(MCW,-997);
	}
	long should_be_totaly = -1;
	MPI_Allreduce( &ny, &should_be_totaly, 1, MPI_LONG, MPI_SUM, MCW);
	if ( should_be_totaly != totaly ) {
		fprintf(stdout,"Incorrect row count in linearpart (%ld instead of %ld).\n",
			should_be_totaly, totaly);
		fflush(stdout);
		MPI_Abort(MCW,-998);
	}

	dxA = dx_in;
	dyA = dy_in;
	MPI_type = MPIt;
	noData = nd;

	//Allocate memory for data and fill with noData value.  Catch exceptions
	uint64_t prod;  //   use long 64 bit number to hold the product to allocate
	try
	{
		prod=nx*ny;
		gridData = new datatype[prod];
		topBorder = new datatype[nx];
		bottomBorder = new datatype[nx];
		passBordersBuffer = new datatype[nx];
	}
	catch(bad_alloc&)
	{
	//  DGT added clause below to try trap for insufficient memory in the computer.
		fprintf(stdout,"Memory allocation error during partition initialization in process %d.\n",rank);
		fprintf(stdout,"NCols: %ld, NRows: %ld, NCells: %ld\n",nx,ny,prod);
		fflush(stdout);
		MPI_Abort(MCW,-999);
	}

	for(uint64_t j=0; j<nx; j++){
		for(uint64_t i=0; i<ny; i++) gridData[i*nx+j] = noData;
		topBorder[j] = noData;
		bottomBorder[j] = noData;
	}

	//TODO: find out what these are for
	after1=after2=before1=before2=NULL;
}


//Returns true if (x,y) is in partition
template <class datatype>
bool linearpart<datatype>::isInPartition(int x, int y) {
	if(x>=0 && x<nx && y>=0 && y<ny) return true;
	else return false;
}

//Returns true if (x,y) is in or on borders of partition
template <class datatype>
bool linearpart<datatype>::hasAccess( int x, int y) {
	//isInPartition takes care of the case where (x,y) is inside the grid
	if(x>=0 && x<nx && y>=0 && y<ny) return true;   // DGT reducing function nesting for efficiency
//	if(isInPartition(x,y)) return true;	

	//Now we only need to worry about borders.
	//x must be bounded by 0 and nx, y may be -1 to ny
	else if(x>=0 && x<nx ) {
		if(rank !=0 && y==-1) return true;
		if(rank !=size-1 && y==ny) return true;
	}
	return false;
}

//Shares border information between adjacent processes.  Border information is stored
//in the "topBorder" and "bottomBorder" arrays of each process.
template <class datatype>
void linearpart<datatype>::share() {
	MPI_Status status;
	if(size<=1) return; //if there is only one process, we're all done sharing

	if (rank<size-1 && rank>0) {
		MPI_Sendrecv(gridData+((ny-1)*nx), nx, MPI_type, rank+1, 0,
			topBorder, nx, MPI_type, rank-1, 0, MCW, &status);
		MPI_Sendrecv(gridData, nx, MPI_type, rank-1, 0,
			bottomBorder, nx, MPI_type, rank+1, 0, MCW, &status);
	} else if (rank<size-1) {
		MPI_Send(gridData+((ny-1)*nx), nx, MPI_type, rank+1, 0, MCW);
		MPI_Recv(bottomBorder, nx, MPI_type, rank+1, 0, MCW, &status);
	} else if (rank>0) {
		MPI_Recv(topBorder, nx, MPI_type, rank-1, 0, MCW, &status);
		MPI_Send(gridData, nx, MPI_type, rank-1, 0, MCW);
	}
}

//Swaps border information between adjacent processes.  In this way, no data is
//overwritten.  If this function is called a second time, the original state is
//restored.
template <class datatype>
void linearpart<datatype>::passBorders() {
	MPI_Status status;
	if(size<=1) return; //if there is only one process, we're all done sharing

	//  0 exchange with 1, 2 exchange with 3, ...
	if (rank%2==0 && rank+1<size) {
		std::swap(bottomBorder,passBordersBuffer);
		MPI_Sendrecv(passBordersBuffer, nx, MPI_type, rank+1, 0,
			bottomBorder, nx, MPI_type, rank+1, 0, MCW, &status);
	}
	if (rank%2==1) {
		std::swap(topBorder,passBordersBuffer);
		MPI_Sendrecv(passBordersBuffer, nx, MPI_type, rank-1, 0,
			topBorder, nx, MPI_type, rank-1, 0, MCW, &status);
	}

	//  1 exchange with 2, 3 exchange with 4, ...
	if (rank%2==1 && rank+1<size) {
		std::swap(bottomBorder,passBordersBuffer);
		MPI_Sendrecv(passBordersBuffer, nx, MPI_type, rank+1, 0,
			bottomBorder, nx, MPI_type, rank+1, 0, MCW, &status);
	}
	if (rank%2==0 && rank>0) {
		std::swap(topBorder,passBordersBuffer);
		MPI_Sendrecv(passBordersBuffer, nx, MPI_type, rank-1, 0,
			topBorder, nx, MPI_type, rank-1, 0, MCW, &status);
	}
}

//Swaps border information between adjacent processes,
//then adds the values from received borders to the local copies.
template <class datatype>
void linearpart<datatype>::addBorders(){
	//Start by calling passBorders to get information.
	passBorders();

	uint64_t i;
	for(i=0; i<nx; i++){
		//Add the values passed in from other process
		if(isNodata(i,-1) || isNodata(i,0)) setData(i, 0, noData);
		else addToData(i, 0, topBorder[i]);

		if(isNodata(i, ny) || isNodata(i, ny-1)) setData(i, ny-1, noData);
		else addToData(i, ny-1, bottomBorder[i]);

	}
}

//Clears borders (sets them to zero).
template <class datatype>
void linearpart<datatype>::clearBorders(){
	uint64_t i;
	for(i=0; i<nx; i++){
		topBorder[i] = 0;
		bottomBorder[i] = 0;
	}
}


template <class datatype>
int linearpart<datatype>::ringTerm(int isFinished) {
	//The parameter isFinished tells us if the que is empty.
	int ringBool = 1;
	MPI_Allreduce( &isFinished, &ringBool, 1, MPI_INT, MPI_LAND, MCW);
	return ringBool; 
}

//Converts global coordinates (for the whole grid) to local coordinates (for this
//partition).  Function returns TRUE only if the coordinates are contained
//in this partition.
template <class datatype>
bool linearpart<datatype>::globalToLocal(int globalX, int globalY, int &localX, int &localY){
	localX = globalX;
	localY = globalY - starty;
	return isInPartition(localX, localY);
} 

//Converts local coordinates (for this partition) to the whole grid.
template <class datatype>
void linearpart<datatype>::localToGlobal(int localX, int localY, int &globalX, int &globalY){
	globalX = localX;
	globalY = starty + localY;
}

//TODO: Revisit this function to see how necessary it is.
//It only gets called a couple of times throughout Taudem.
template <class datatype>
void linearpart<datatype>::transferPack( int *countA, int *bufferAbove, int *countB, int *bufferBelow) {
	MPI_Status status;
	if(size==1) return;

	int place;
	datatype *abuf, *bbuf;
	int absize;
	int bbsize;
	absize=*countA*sizeof(int)+MPI_BSEND_OVERHEAD;  
	bbsize=*countB*sizeof(int)+MPI_BSEND_OVERHEAD;  
		
	abuf = new datatype[absize];
	bbuf = new datatype[bbsize];
	//MPI_Buffer_attach(buf,bbsize);

	if( rank >0 ) {
		MPI_Buffer_attach(abuf,absize);
		MPI_Bsend( bufferAbove, *countA, MPI_INT, rank-1, 3, MCW );
		MPI_Buffer_detach(&abuf,&place);
	}
	if( rank < size-1) {
		MPI_Probe( rank+1,3,MCW, &status);  // Blocking function this only returns when there is a message to receive
		MPI_Get_count( &status, MPI_INT, countA);  //  To get count from the status variable
		MPI_Recv( bufferAbove, *countA,MPI_INT, rank+1,3,MCW,&status);  // Receives message sent in first if from another process
		MPI_Buffer_attach(bbuf,bbsize);
		MPI_Bsend( bufferBelow, *countB, MPI_INT, rank+1,3,MCW);
		MPI_Buffer_detach(&bbuf,&place);
	}
	if( rank > 0 ) {
		MPI_Probe( rank-1,3,MCW, &status);
		MPI_Get_count( &status, MPI_INT, countB);
		MPI_Recv( bufferBelow, *countB,MPI_INT, rank-1,3,MCW,&status);
	}

	delete abuf;
	delete bbuf;
}

//Returns true if grid element (x,y) is equal to noData.
template <class datatype>
bool linearpart<datatype>::isNodata(long inx, long iny){
	int64_t x, y;//int64 because it oculd be -1.
	x = inx;
	y = iny;
//DGT to avoid nested calls and type inconsistency
	if(x>=0 && x<nx && y>=0 && y<ny)return (abs((float)(gridData[x+y*nx]-noData))<MINEPS);  
//	if(isInPartition(x,y)) return (abs(gridData[x+y*nx]-noData)<MINEPS);
	else if(x>=0 && x<nx){
		if(y==-1) return (abs((float)(topBorder[x]-noData))<MINEPS);
		else if(y==ny) return (abs((float)(bottomBorder[x]-noData))<MINEPS);
	}
	return true;
}

//Sets the element in the grid to noData.
template <class datatype>
void linearpart<datatype>::setToNodata(long inx, long iny){
	int64_t x, y;
	x = inx;
	y = iny;
	if(x>=0 && x<nx && y>=0 && y<ny) gridData[x+y*nx] = noData;
//	if(isInPartition(x,y)) gridData[x+y*nx] = noData;
	else if(x>=0 && x<nx){
		if(y==-1) topBorder[x] = noData;
		else if(y==ny) bottomBorder[x] = noData;
	}
}

//Returns the element in the grid with coordinate (x,y).
template <class datatype>
datatype linearpart<datatype>::getData(long inx, long iny, datatype &val) {
	int64_t x, y;
	x = inx;
	y = iny;
//	if(isInPartition(x,y)) val = gridData[x+y*nx];
	if(x>=0 && x<nx && y>=0 && y<ny) val = gridData[x+y*nx];
	else if(x>=0 && x<nx){
		if(y==-1) val = topBorder[x];
		else if(y==ny) val = bottomBorder[x];
	}
	return val;
}



template <class datatype>
void linearpart<datatype>::savedxdyc( tiffIO &obj) {
	dxc = new double[ny+2];
	dyc = new double[ny+2];
	int ilo = ( rank == 0 ? 0 : -1 );
	int ihi = ( rank == size-1 ? ny : ny+1 );
	for ( int i = ilo; i < ihi; i++ ) {
		int globalX, globalY;
		localToGlobal(0, i, globalX, globalY);
		dxc[i+1] = obj.getdxc(globalY);
		dyc[i+1] = obj.getdyc(globalY);
	}
}
	

template <class datatype>
void linearpart<datatype>::getdxdyc(long iny, double &val_dxc,double &val_dyc){
	 int64_t y;y = iny;
	 if ( y >= -1 && y < ny+1 ) { val_dxc = dxc[y+1]; val_dyc = dyc[y+1]; }
}



//Sets the element in the grid to the specified value.
template <class datatype>
void linearpart<datatype>::setData(long inx, long iny, datatype val){
	int64_t x, y;
	x = inx;
	y = iny;
//	if(isInPartition(x,y)) gridData[x+y*nx] = val;
	if(x>=0 && x<nx && y>=0 && y<ny) gridData[x+y*nx] = val;
	else if(x>=0 && x<nx){
		if(y==-1) topBorder[x] = val;
		else if(y==ny) bottomBorder[x] = val;
	}
}

//Increments the element in the grid by the specified value.
template <class datatype>
void linearpart<datatype>::addToData(long inx, long iny, datatype val){
	int64_t x, y;
	x = inx;
	y = iny;
//	if(isInPartition(x,y)) gridData[x+y*nx] += val;
	if(x>=0 && x<nx && y>=0 && y<ny) gridData[x+y*nx] += val;
	else if(x>=0 && x<nx){
		if(y==-1) topBorder[x] += val;
		else if(y==ny) bottomBorder[x] += val;
	}
}
#endif
